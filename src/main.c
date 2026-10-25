/*
 * main.c - command line front end.
 *
 *   adiff [options] OLD NEW        diff two files
 *   adiff bench  OLD NEW           time and memory of every engine
 *   adiff gen    ...               make synthetic test files
 *   adiff selftest                 cross-check all engines on random inputs
 */
#define _POSIX_C_SOURCE 200809L
#include "adiff.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if !defined(_WIN32)
#include <sys/resource.h>
#endif

static void usage(void) {
    puts(
"Adaptive LCS Diff - memory-aware, move-aware diff built on LCS dynamic programming\n"
"\n"
"usage:\n"
"  adiff [options] OLD NEW        diff two files (exit 0 = same, 1 = differ)\n"
"  adiff bench [options] OLD NEW  compare engines on time and peak memory\n"
"  adiff gen [options] OLD NEW    write two synthetic files to test with\n"
"  adiff selftest                 check all engines against each other\n"
"\n"
"diff options:\n"
"  --engine E     auto (default), classic, full, hirschberg, banded, myers\n"
"  --no-moves     do not detect moved blocks\n"
"  --no-tokens    do not mark changed tokens inside changed lines\n"
"  --move-min N   smallest block reported as a move (default 3)\n"
"  --context N    lines of context around a change (default 3)\n"
"  --color        use ANSI colors\n"
"  --stats        print engine, sizes, time and peak memory to stderr\n"
"  --verify       check that old file + script == new file\n"
"\n"
"output legend:\n"
"  ' ' unchanged   '-' deleted   '+' inserted\n"
"  '<' moved out of here   '>' moved in here\n"
"  [-text-] / {+text+} the tokens that changed inside a modified line");
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static double peak_rss_mb(void) {
#if defined(_WIN32)
    return 0.0;
#else
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return (double)ru.ru_maxrss / (1024.0 * 1024.0);
#else
    return (double)ru.ru_maxrss / 1024.0;
#endif
#endif
}

static int cmd_diff(int argc, char **argv) {
    Engine eng = ENG_AUTO;
    int moves = 1, tokens = 1, ctx = 3, color = 0, stats = 0, verify = 0, move_min = 3;
    const char *files[2] = { NULL, NULL };
    int nf = 0;

    for (int i = 0; i < argc; i++) {
        const char *s = argv[i];
        if (!strcmp(s, "--engine") && i + 1 < argc) {
            if (engine_parse(argv[++i], &eng) != 0) { fprintf(stderr, "adiff: unknown engine '%s'\n", argv[i]); return 2; }
        } else if (!strcmp(s, "--no-moves")) moves = 0;
        else if (!strcmp(s, "--no-tokens")) tokens = 0;
        else if (!strcmp(s, "--move-min") && i + 1 < argc) move_min = atoi(argv[++i]);
        else if (!strcmp(s, "--context") && i + 1 < argc) ctx = atoi(argv[++i]);
        else if (!strcmp(s, "--color")) color = 1;
        else if (!strcmp(s, "--stats")) stats = 1;
        else if (!strcmp(s, "--verify")) verify = 1;
        else if (s[0] == '-' && s[1] != '\0') { fprintf(stderr, "adiff: unknown option '%s'\n", s); return 2; }
        else if (nf < 2) files[nf++] = s;
        else { fprintf(stderr, "adiff: too many files\n"); return 2; }
    }
    if (nf != 2) { usage(); return 2; }

    Pair p;
    if (pair_load(&p, files[0], files[1]) != 0) return 2;

    MatchList ml;
    ml_init(&ml);
    EngineInfo info;
    double t0 = now_ms();
    int rc = compute_matches(p.a.id, p.a.n, p.b.id, p.b.n, p.nids, eng, &ml, &info);
    double t1 = now_ms();
    if (rc != 0) {
        fprintf(stderr, "adiff: engine '%s' hit its memory limit. Try --engine hirschberg or auto.\n",
                engine_name(info.used));
        pair_free(&p);
        return 2;
    }

    unsigned char *ma = calloc((size_t)p.a.n + 1, 1);
    unsigned char *mb = calloc((size_t)p.b.n + 1, 1);
    for (int t = 0; t < ml.n; t++) { ma[ml.v[t].a] = 1; mb[ml.v[t].b] = 1; }

    MoveList mv = { NULL, 0, 0 };
    double t2 = now_ms();
    if (moves) detect_moves(p.a.id, p.a.n, p.b.id, p.b.n, p.trivial, ma, mb, move_min, &mv);
    double t3 = now_ms();

    OpList ops;
    build_ops(&p, &ml, &ops);
    if (moves) annotate_moves(&ops, &mv, p.a.n, p.b.n);
    if (tokens) annotate_tokens(&p, &ops);
    double t4 = now_ms();

    int status = 0;
    if (verify) {
        char err[128];
        if (verify_script(&p, &ml, &ops, err, sizeof err) != 0) {
            fprintf(stderr, "adiff: VERIFY FAILED: %s\n", err);
            status = 2;
        } else {
            fprintf(stderr, "adiff: verify OK (old file + script == new file)\n");
        }
    }

    int changed = ops_have_changes(&ops);
    if (changed && status == 0) {
        print_diff(stdout, &p, &ops, moves ? &mv : NULL, files[0], files[1], ctx, color);
        status = 1;
    }

    if (stats) {
        int dist = p.a.n + p.b.n - 2 * ml.n;
        fprintf(stderr,
                "engine        : %s%s\n"
                "lines         : old %d, new %d (trimmed %d head + %d tail, core %d x %d)\n"
                "common lines  : %d  (edit distance %d)\n"
                "moved blocks  : %d\n"
                "time          : LCS %.3f ms, moves %.3f ms, script+tokens %.3f ms\n"
                "peak memory   : %.1f MB\n",
                engine_name(info.used), info.band_k ? " (banded)" : "",
                p.a.n, p.b.n, info.trim_prefix, info.trim_suffix, info.core_m, info.core_n,
                ml.n, dist, mv.n, t1 - t0, t3 - t2, t4 - t3, peak_rss_mb());
        if (info.band_k) fprintf(stderr, "band width    : %d\n", info.band_k);
    }

    ops_free(&ops);
    moves_free(&mv);
    ml_free(&ml);
    free(ma);
    free(mb);
    pair_free(&p);
    return status;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) { usage(); return 0; }
    if (!strcmp(argv[1], "bench")) return cmd_bench(argc - 2, argv + 2);
    if (!strcmp(argv[1], "gen")) return cmd_gen(argc - 2, argv + 2);
    if (!strcmp(argv[1], "selftest")) return cmd_selftest(argc - 2, argv + 2);
    int off = !strcmp(argv[1], "diff") ? 2 : 1;
    return cmd_diff(argc - off, argv + off);
}
