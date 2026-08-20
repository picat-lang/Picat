/*  satshim - CNF feeder / protocol bridge for Picat's external SAT
 *  solver runner (emu/satext.c). Linux.
 *
 *  Why it exists: for large CNFs (millions of clauses) the VM must
 *  not do int->ASCII formatting, and the CNF should not cross a pipe
 *  byte-by-byte from the VM. The parent process stages the CNF as
 *  binary int32 clauses in a memfd and hands the descriptor over via
 *  SCM_RIGHTS (a socketpair created before the fork; the child
 *  receives it as fd 3). This program mmaps the shared image, formats
 *  DIMACS text, and feeds a forked solver:
 *
 *      solver stdin  <- formatted CNF ("solve" line appended in
 *                       IPASIR mode)
 *      solver stdout -> outfd (the parent's read pipe)
 *      solver stderr -> unchanged (parent's stderr, so solver
 *                       progress stays visible to the user)
 *
 *  Usage (invoked by the parent, not by users):
 *    satshim bin  <0|1> <outfd> <solver> <args...>
 *        binary int32 CNF arrives on fd 3; <0|1> = IPASIR flag
 *    satshim file <0|1> <outfd> <srcpath> <solver> <args...>
 *        read a text DIMACS/IPASIR file; <0|1> = IPASIR flag
 *
 *  Binary layout on fd 3 (native endianness; same-machine handoff):
 *      int32  magic (0x46415049 "'IPAF' byte-swapped", see below)
 *      int64  nclauses
 *      int64  nvars
 *      int64  nlits
 *      int32  literals..., each clause terminated by 0
 *
 *  Exit status: the solver's exit status; 77 for feeder failures.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define SHIM_BUF_SZ (1u << 20)
#define MAGIC (int32_t)0x46415049

/* Same contract as in satext.c: the shim's solver child must die if
   the shim dies (the shim itself dies when picat dies, see
   child_dies_with_parent there). */
static void child_dies_with_parent(void)
{
    (void)prctl(PR_SET_PDEATHSIG, SIGKILL);
}

static int recv_fd(int sd)
{
    struct msghdr m;
    struct iovec io;
    char one = 0;
    char cbuf[CMSG_SPACE(sizeof(int))];
    int fd = -1;

    memset(&m, 0, sizeof(m));
    io.iov_base = &one;
    io.iov_len = 1;
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    m.msg_control = cbuf;
    m.msg_controllen = sizeof(cbuf);
    if (recvmsg(sd, &m, 0) <= 0) return -1;
    if (m.msg_controllen < CMSG_LEN(sizeof(int))) return -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_type == SCM_RIGHTS && c->cmsg_len >= CMSG_LEN(sizeof(int))) {
            memcpy(&fd, CMSG_DATA(c), sizeof(int));
            break;
        }
    return fd;
}

static int write_all(int fd, const void *p, size_t n)
{
    const char *q = p;
    size_t r = n;
    while (r > 0) {
        ssize_t w = write(fd, q, r);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        q += w;
        r -= (size_t)w;
    }
    return 0;
}

static size_t put_lit(char *b, size_t pos, int32_t v)
{
    char tmp[16];
    int len;
    uint32_t u;

    if (v < 0) {
        b[pos++] = '-';
        u = (uint32_t)(-(int64_t)v);
    } else {
        u = (uint32_t)v;
    }
    len = 0;
    do {
        tmp[len++] = (char)('0' + (u % 10u));
        u /= 10u;
    } while (u > 0u);
    while (len > 0) b[pos++] = tmp[--len];
    b[pos++] = ' ';
    return pos;
}

static size_t put_64(char *b, size_t pos, int64_t v)
{
    char tmp[24];
    int len = 0;
    uint64_t u = (uint64_t)v;
    do {
        tmp[len++] = (char)('0' + (u % 10u));
        u /= 10u;
    } while (u > 0u);
    while (len > 0) b[pos++] = tmp[--len];
    return pos;
}

/*
 * Format and write the binary CNF (image, len) into fd cnf_wr.
 * Returns 0 on success, a distinct non-zero code on failure.
 */
static int feed_binary(const uint8_t *img, size_t len, int cnf_wr, int ipasir)
{
    int32_t magic;
    int64_t ncl = 0, nv = 0, nl = 0, off;
    char *buf;
    int rc = 0;

    if (len < 28) return 71;
    memcpy(&magic, img + 0, 4);
    if (magic != (int32_t)MAGIC) return 72;
    memcpy(&ncl, img + 4, 8);
    memcpy(&nv, img + 12, 8);
    memcpy(&nl, img + 20, 8);

    buf = (char *)malloc(SHIM_BUF_SZ);
    if (!buf) return 73;

    size_t pos = 6;
    memcpy(buf, "p cnf ", 6);
    pos = put_64(buf, pos, nv);
    buf[pos++] = ' ';
    pos = put_64(buf, pos, ncl);
    buf[pos++] = '\n';
    rc = write_all(cnf_wr, buf, pos);

    if (rc == 0) {
        off = 28;
        for (int64_t i = 0; i < ncl && rc == 0; i++) {
            int terminated = 0;
            pos = 0;
            while (!terminated) {
                int32_t v;
                if (off + 4 > len) { rc = 74; break; }
                memcpy(&v, img + off, 4);
                off += 4;
                if (v == 0) {
                    pos = put_lit(buf, pos, 0);
                    buf[pos++] = '\n';
                    terminated = 1;
                    break;
                }
                if (pos >= SHIM_BUF_SZ - 16) {
                    rc = write_all(cnf_wr, buf, pos);
                    pos = 0;
                    if (rc != 0) break;
                }
                pos = put_lit(buf, pos, v);
            }
            if (rc == 0 && pos > 0) rc = write_all(cnf_wr, buf, pos);
        }
    }
    if (rc == 0 && ipasir) rc = write_all(cnf_wr, "solve\n", 6);
    free(buf);
    return rc;
}

static int feed_file(const char *path, int cnf_wr, int ipasir)
{
    FILE *f;
    char *buf;
    size_t r;
    int rc = 0;

    f = fopen(path, "rb");
    if (!f) return 75;
    buf = (char *)malloc(SHIM_BUF_SZ);
    if (!buf) { fclose(f); return 73; }
    while ((r = fread(buf, 1, SHIM_BUF_SZ, f)) > 0) {
        if (write_all(cnf_wr, buf, r) != 0) { rc = 76; break; }
    }
    free(buf);
    fclose(f);
    if (rc == 0 && ipasir) rc = write_all(cnf_wr, "solve\n", 6);
    return rc;
}

/*
 * Run the solver: solver stdin <- feeder pipe, stdout -> outfd,
 * stderr -> inherited. Wait for it and exit with its status.
 */
int main(int argc, char **argv)
{
    int mode_bin, ipasir, outfd, i;
    int cnf[2], sv;
    pid_t pid;
    int feeder_rc = 0;
    char *solver;
    char **solver_argv;

    if (argc < 5) { fprintf(stderr, "satshim: bad usage\n"); return 77; }
    if (strcmp(argv[1], "bin") == 0) {
        mode_bin = 1; ipasir = atoi(argv[2]);
        outfd = atoi(argv[3]); solver = argv[4];
    } else if (strcmp(argv[1], "file") == 0) {
        mode_bin = 0; ipasir = atoi(argv[2]);
        outfd = atoi(argv[3]);
        /* srcpath carried via argv[4], solver at argv[5] */
        solver = argv[5];
    } else {
        fprintf(stderr, "satshim: unknown mode %s\n", argv[1]);
        return 77;
    }
    if (argc <= (mode_bin ? 4 : 5)) {
        fprintf(stderr, "satshim: no solver argv\n");
        return 77;
    }

    if (mode_bin) {
        int fd3 = recv_fd(3);
        uint8_t *img;
        size_t len;
        if (fd3 < 0) { fprintf(stderr, "satshim: no fd on socket\n"); return 77; }
        {
            struct stat st;
            if (fstat(fd3, &st) != 0 || st.st_size < 28) {
                fprintf(stderr, "satshim: bad CNF fd\n"); return 77;
            }
            len = (size_t)st.st_size;
        }
        img = mmap(NULL, len, PROT_READ, MAP_SHARED, fd3, 0);
        close(fd3);
        if (img == MAP_FAILED) { fprintf(stderr, "satshim: mmap fail\n"); return 77; }

        if (pipe(cnf) != 0) { munmap(img, len); return 77; }
        pid = fork();
        if (pid < 0) { close(cnf[0]); close(cnf[1]); munmap(img, len); return 77; }
        if (pid == 0) {
            /* solver child of the shim: stdin <- read end of the CNF pipe */
            child_dies_with_parent();
            dup2(cnf[0], 0);
            dup2(outfd, 1);
            close(cnf[0]); close(cnf[1]); close(outfd);
            close(3);
            solver_argv = &argv[4];
            execvp(solver, solver_argv);
            fprintf(stderr, "satshim: exec %s failed\n", solver);
            _exit(127);
        }
        close(cnf[0]);
        feeder_rc = feed_binary(img, len, cnf[1], ipasir);
        munmap(img, len);
        close(cnf[1]);
    } else {
        const char *srcpath = argv[4];
        if (pipe(cnf) != 0) return 77;
        pid = fork();
        if (pid < 0) { close(cnf[0]); close(cnf[1]); return 77; }
        if (pid == 0) {
            child_dies_with_parent();
            dup2(cnf[0], 0);
            dup2(outfd, 1);
            close(cnf[0]); close(cnf[1]); close(outfd);
            close(3);
            solver_argv = &argv[5];
            execvp(solver, solver_argv);
            fprintf(stderr, "satshim: exec %s failed\n", solver);
            _exit(127);
        }
        close(cnf[0]);
        feeder_rc = feed_file(srcpath, cnf[1], ipasir);
        close(cnf[1]);
    }

    if (feeder_rc != 0) {
        fprintf(stderr, "satshim: feeder error %d\n", feeder_rc);
        kill(pid, 9);
        waitpid(pid, NULL, 0);
        return feeder_rc;
    }

    {
        int st;
        waitpid(pid, &st, 0);
        if (WIFEXITED(st)) return WEXITSTATUS(st);
        if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
        return 77;
    }
}
