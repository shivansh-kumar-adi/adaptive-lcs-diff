/*
 * bench.c - time and peak memory of every engine on one pair of files.
 *
 * Every measurement runs in its OWN child process, so the peak memory of one
 * engine can never hide behind the peak of another. A "baseline" child that
 * only loads the files is measured first and subtracted, which leaves the
 * memory used by the algorithm itself.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "adiff.h"
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

int cmd_bench(int argc, char **argv) {
    (void)argc; (void)argv;
    fprintf(stderr, "adiff bench needs fork() and wait4(); run it on Linux, macOS or WSL.\n");
    return 2;
}

#else

#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct { int status; int lcs; int used; double ms; } ChildRes;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int count_lines_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char buf[1 << 16];
    int n = 0, last = '\n';
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0)
        for (size_t i = 0; i < r; i++) {
            if (buf[i] == '\n') n++;
            last = buf[i];
        }
    fclose(f);
    if (last != '\n') n++;
    return n;
}

/* eng < 0: baseline child that only loads the files */
static int run_child(const char *oldp, const char *newp, int eng, ChildRes *res, double *rss_mb) {
    int fd[2];
    if (pipe(fd) != 0) return -1;
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(fd[0]);
        ChildRes r;
        memset(&r, 0, sizeof r);
        Pair p;
        if (pair_load(&p, oldp, newp) != 0) {
            r.status = -2;
        } else if (eng >= 0) {
            MatchList ml;
            ml_init(&ml);
            EngineInfo info;
            double t0 = now_ms();
            int rc = compute_matches(p.a.id, p.a.n, p.b.id, p.b.n, p.nids, (Engine)eng, &ml, &info);
            double t1 = now_ms();
            r.status = rc;
            r.lcs = ml.n;
            r.used = (int)info.used;
            r.ms = t1 - t0;
        }
        if (write(fd[1], &r, sizeof r) < 0) _exit(3);
        _exit(0);
    }
    close(fd[1]);
    int st = 0;
    struct rusage ru;
    memset(&ru, 0, sizeof ru);
    wait4(pid, &st, 0, &ru);
    memset(res, 0, sizeof *res);
    ssize_t got = read(fd[0], res, sizeof *res);
    close(fd[0]);
    if (!WIFEXITED(st) || got != (ssize_t)sizeof *res) res->status = -3;   /* killed, probably out of memory */
#if defined(__APPLE__)
    *rss_mb = (double)ru.ru_maxrss / (1024.0 * 1024.0);
#else
    *rss_mb = (double)ru.ru_maxrss / 1024.0;
#endif
    return 0;
}

static int dcmp(const void *x, const void *y) {
    double a = *(const double *)x, b = *(const double *)y;
    return (a > b) - (a < b);
}

int cmd_bench(int argc, char **argv) {
    int runs = 5;
    const char *list = "classic,full,hirschberg,banded,myers,auto";
    const char *files[2] = { NULL, NULL };
    int nf = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--runs") && i + 1 < argc) runs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--engines") && i + 1 < argc) list = argv[++i];
        else if (argv[i][0] == '-' && argv[i][1]) { fprintf(stderr, "adiff bench: unknown option '%s'\n", argv[i]); return 2; }
        else if (nf < 2) files[nf++] = argv[i];
    }
    if (nf != 2 || runs < 1) {
        fprintf(stderr, "usage: adiff bench [--runs N] [--engines a,b,c] OLD NEW\n");
        return 2;
    }
    int m = count_lines_file(files[0]), n = count_lines_file(files[1]);
    if (m < 0 || n < 0) { fprintf(stderr, "adiff bench: cannot read input files\n"); return 2; }

    /* baseline memory: load only */
    double base = 0;
    for (int r = 0; r < runs; r++) {
        ChildRes cr;
        double mb = 0;
        if (run_child(files[0], files[1], -1, &cr, &mb) != 0) return 2;
        if (mb > base) base = mb;
    }

    printf("old: %d lines   new: %d lines   runs per engine: %d\n", m, n, runs);
    printf("baseline (files loaded, no algorithm): %.1f MB\n\n", base);
    printf("%-11s %-11s %9s %9s %12s %14s\n", "engine", "ran as", "common", "distance", "time (ms)", "extra mem (MB)");
    printf("%-11s %-11s %9s %9s %12s %14s\n", "-----------", "-----------", "---------", "---------", "------------", "--------------");

    char buf[256];
    strncpy(buf, list, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        Engine e;
        if (engine_parse(tok, &e) != 0) { printf("%-11s unknown engine\n", tok); continue; }
        double *ts = malloc(sizeof(double) * (size_t)runs);
        double peak = 0;
        ChildRes last;
        int failed = 0;
        memset(&last, 0, sizeof last);
        for (int r = 0; r < runs; r++) {
            ChildRes cr;
            double mb = 0;
            if (run_child(files[0], files[1], (int)e, &cr, &mb) != 0) { failed = -9; break; }
            if (cr.status != 0) { failed = cr.status; last = cr; break; }
            ts[r] = cr.ms;
            if (mb > peak) peak = mb;
            last = cr;
        }
        if (failed) {
            printf("%-11s %-11s %9s %9s %12s %14s\n", tok, "-", "-", "-", "-",
                   failed == -3 ? "killed (memory)" : "over memory limit");
        } else {
            qsort(ts, (size_t)runs, sizeof(double), dcmp);
            double med = ts[runs / 2];
            double extra = peak - base;
            if (extra < 0) extra = 0;
            printf("%-11s %-11s %9d %9d %12.2f %14.1f\n", tok, engine_name((Engine)last.used), last.lcs,
                   m + n - 2 * last.lcs, med, extra);
        }
        free(ts);
    }
    printf("\ncommon = lines kept (LCS length), distance = insertions + deletions. All engines must agree.\n");
    return 0;
}

#endif
