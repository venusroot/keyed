#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/user.h>
#include <sys/wait.h>

#include <syscall.h>
#include <sys/reg.h>
#include <sys/uio.h>
#include <sys/ptrace.h>

#include <sodium.h>

#define countof(a) (sizeof(a) / sizeof(0[a]))

#define PASSPHRASE_MAX 1024

__attribute__((noreturn))
static void
fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "keyed: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(EXIT_FAILURE);
}

static void
get_passphrase(char *buf, size_t len, const char *prompt)
{
    /* Open and configure /dev/tty */
    int tty = open("/dev/tty", O_RDWR);
    if (tty == -1) {
        fatal("/dev/tty: %s", strerror(errno));
    }
    struct termios old, new;
    tcgetattr(tty, &old);
    new = old;
    new.c_lflag &= ~ECHO;
    tcsetattr(tty, TCSANOW, &new);

    if (write(tty, prompt, strlen(prompt)) == -1)
        fatal("/dev/tty: %s", strerror(errno));

    /* Read and process the passphrase */
    ssize_t r = read(tty, buf, len);
    if (r == -1)
        fatal("/dev/tty: %s", strerror(errno));
    if (r == (ssize_t)len)
        fatal("passphrase too long");
    char *s = buf;
    for (; s < buf + r; s++)
        if (*s == '\n')
            break;
    *s = 0;

    /* Restore and close /dev/tty */
    static const char newline = '\n';
    tcsetattr(tty, TCSANOW, &old);
    if (write(tty, &newline, 1) == -1)
        fatal("/dev/tty: %s", strerror(errno));
    close(tty);
}

static void
usage(FILE *f)
{
    fputs("usage: keyed [-hv] [-n n] [-k file] [-p[pid]] command [args]\n", f);
    fputs("  -h        print this messsage\n", f);
    fputs("  -k file   read passphrase from a file\n", f);
    fputs("  -n int    number of times to repeat passphrase prompt (1)\n", f);
    fputs("  -p[pid]   also intercept getpid() syscalls\n", f);
    fputs("  -v        verbose messages\n", f);
}

int
main(int argc, char **argv)
{
    int verbose = 0;
    char *keyfile = 0;
    long fake_pid = -1;
    int passphrase_repeat = 1;

    int option;
    while ((option = getopt(argc, argv, "+hk:n:p::v")) != -1) {
        switch (option) {
            case 'h':
                usage(stdout);
                exit(EXIT_SUCCESS);
            case 'k':
                keyfile = optarg;
                break;
            case 'n':
                passphrase_repeat = strtol(optarg, 0, 10);
                break;
            case 'p':
                fake_pid = optarg ? atoi(optarg) : 2;
                break;
            case 'v':
                verbose++;
                break;
            default:
                usage(stderr);
                exit(EXIT_FAILURE);
        }
    }

    char passphrase[PASSPHRASE_MAX];
    if (keyfile) {
        /* Load passphrase from a file */
        FILE *f = fopen(keyfile, "rb");
        if (!f)
            fatal("%s", strerror(errno));
        size_t in = fread(passphrase, 1, sizeof(passphrase), f);
        if (!in && ferror(f))
            fatal("failed to read passphrase: %s", keyfile);
        if (in == sizeof(passphrase))
            fatal("passphase too long");
        char *s = passphrase;
        for (; s < passphrase + in; s++)
            if (*s == '\n')
                break;
        *s = 0;
        fclose(f);
    } else {
        /* Prompt user for a passphrase */
        static const char prompt[] = "passphrase: ";
        static const char again[]  = "passphrase (again): ";
        get_passphrase(passphrase, sizeof(passphrase), prompt);
        for (int i = 0; i < passphrase_repeat; i++) {
            char check[PASSPHRASE_MAX];
            get_passphrase(check, sizeof(check), again);
            if (strcmp(passphrase, check))
                fatal("passphrases don't match");
        }
    }

    /* Derive a key from the passphrase */
    unsigned char salt[crypto_pwhash_SALTBYTES] = {0};
    unsigned char key[crypto_pwhash_scryptsalsa208sha256_STRBYTES];
    unsigned long long opslimit = crypto_pwhash_OPSLIMIT_MODERATE;
    size_t memlimit = crypto_pwhash_MEMLIMIT_MODERATE;
    int alg = crypto_pwhash_ALG_DEFAULT;
    if (crypto_pwhash(key, sizeof(key), passphrase, strlen(passphrase),
                      salt, opslimit, memlimit, alg) != 0) {
        fputs("not enough memory to derive key", stderr);
        exit(EXIT_FAILURE);
    }

    /* Exec the given command */
    pid_t pid = fork();
    switch (pid) {
        case -1: /* error */
            fatal("%s", strerror(errno));
        case 0:  /* child */
            ptrace(PTRACE_TRACEME, 0, 0, 0);
            execvp(argv[optind], argv + optind);
            fatal("%s", strerror(errno));
        default:
            waitpid(pid, 0, 0);
            ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_EXITKILL);
    }

    int nmonitor = 0;
    int monitor_fd[16];
    size_t buflen = 0;
    unsigned char *buf = 0;

    for (;;) {
        /* Enter next system call */
        if (ptrace(PTRACE_SYSCALL, pid, 0, 0) == -1)
            fatal("%s", strerror(errno));
        if (waitpid(pid, 0, 0) == -1)
            fatal("%s", strerror(errno));

        /* Gather system call arguments */
        struct user_regs_struct regs;
        if (ptrace(PTRACE_GETREGS, pid, 0, &regs) == -1)
            fatal("%s", strerror(errno));
        long syscall = regs.orig_rax;

        size_t size = 0;
        int capture_fd = 0;
        unsigned long dest;
        switch (syscall) {
            /* Exit along with the child.
             */
            case SYS_exit:
            case SYS_exit_group: {
                exit(regs.rdi);
            } break;

            /* Monitor open(2) for /dev/random and /dev/urandom.
             * Only reads to these file descriptors will be intercepted.
             */
            case SYS_open: {
                char path[13];
                struct iovec liov = {
                    .iov_base = path,
                    .iov_len = sizeof(path)
                };
                struct iovec riov = {
                    .iov_base = (void *)regs.rdi,
                    .iov_len = sizeof(path)
                };
                if (process_vm_readv(pid, &liov, 1, &riov, 1, 0) == -1)
                    fatal("%s", strerror(errno));
                if (!memcmp(path, "/dev/random", 12) ||
                    !memcmp(path, "/dev/urandom", 13)) {
                    if (nmonitor == (int)countof(monitor_fd))
                        fatal("too many open file descriptors");
                    capture_fd = 1;
                }
            } break;

            /* When /dev/random or /dev/urandom is closed, stop
             * monitoring that file descriptor.
             */
            case SYS_close: {
                int fd = regs.rdi;
                for (int i = 0; i < nmonitor; i++) {
                    if (monitor_fd[i] == fd) {
                        monitor_fd[i] = monitor_fd[--nmonitor];
                        if (verbose)
                            fprintf(stderr,"keyed: close(%d)\n", fd);
                        break;
                    }
                }
            } break;

            /* Intercept read(2) to /dev/random and /dev/urandom.
             */
            case SYS_read: {
                int fd = regs.rdi;
                for (int i = 0; i < nmonitor; i++) {
                    if (monitor_fd[i] == fd) {
                        dest = regs.rsi;
                        size = regs.rdx;
                        if (verbose)
                            fprintf(stderr,"keyed: read(%d, 0x%llx, %llu)\n",
                                    fd, regs.rsi, regs.rdx);
                        break;
                    }
                }
            } break;

            /* Intercept all calls to getrandom(2).
             */
            case SYS_getrandom: {
                dest = regs.rdi;
                size = regs.rsi;
                if (verbose)
                    fprintf(stderr,"keyed: getrandom(0x%llx, %llu)\n",
                            regs.rdi, regs.rsi);
            } break;
        }

        /* Caller asked for a non-zero amount of random data */
        if (size) {
            regs.orig_rax = -1; // set to invalid system call
            if (ptrace(PTRACE_SETREGS, pid, 0, &regs) == -1)
                fatal("%s", strerror(errno));
            if (buflen < size) {
                free(buf);
                buflen = size;
                buf = malloc(buflen);
                if (!buf)
                    fatal("out of memory");
            }
        }

        /* Run system call and stop on its exit */
        if (ptrace(PTRACE_SYSCALL, pid, 0, 0) == -1)
            fatal("%s", strerror(errno));
        if (waitpid(pid, 0, 0) == -1)
            fatal("%s", strerror(errno));

        /* Write requested random bytes into child's buffer */
        if (size) {
            static const unsigned char
                nonce[crypto_stream_chacha20_NONCEBYTES] = {0};
            crypto_stream_chacha20(buf, size, nonce, key);
            struct iovec liov = {
                .iov_base = buf,
                .iov_len = size
            };
            struct iovec riov = {
                .iov_base = (void *)dest,
                .iov_len = size
            };
            if (process_vm_writev(pid, &liov, 1, &riov, 1, 0) == -1)
                fatal("%s", strerror(errno));
            if (ptrace(PTRACE_POKEUSER, pid, RAX * 8, size) == -1)
                fatal("%s", strerror(errno));
        }

        /* Track the received file descriptor for read(2) monitoring.
         */
        if (capture_fd) {
            if (ptrace(PTRACE_GETREGS, pid, 0, &regs) == -1)
                fatal("%s", strerror(errno));
            int fd = regs.rax;
            monitor_fd[nmonitor++] = fd;
            if (verbose)
                fprintf(stderr, "keyed: monitoring fd %d\n", fd);
        }

        switch (syscall) {
            /* Provide a fake process ID when asked.
             */
            case SYS_getpid: {
                if (fake_pid != -1) {
                    if (ptrace(PTRACE_POKEUSER, pid, RAX * 8, fake_pid) == -1)
                        fatal("%s", strerror(errno));
                    if (verbose)
                        fprintf(stderr, "keyed: getpid() = %ld\n", fake_pid);
                }
            } break;
        }
    }
}
