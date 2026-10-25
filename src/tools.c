/*
 * tools.c - synthetic test data (gen) and the cross-check test (selftest).
 */
#include "adiff.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* small deterministic random generator (xorshift64*)                  */
/* ------------------------------------------------------------------ */
static uint64_t rng_state = 88172645463325252ULL;
static void rng_seed(uint64_t s) { rng_state = s ? s : 88172645463325252ULL; }
static uint32_t rnd(void) {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 2685821657736338717ULL) >> 32);
}
static double rndf(void) { return (double)(rnd() % 1000000) / 1000000.0; }

/* ------------------------------------------------------------------ */
/* gen: pseudo source code with realistic repetition                   */
/* ------------------------------------------------------------------ */
static char *dupf(const char *fmt, int a, int b, int c) {
    char tmp[160];
    snprintf(tmp, sizeof tmp, fmt, a, b, c);
    return strdup(tmp);
}

static char *make_line(int *uid) {
    static const char *common[] = {
        "", "}", "{", "    return 0;", "    break;", "    continue;", "    /* TODO */",
        "#include <stdio.h>", "#include <stdlib.h>", "    free(ptr);", "    else {"
    };
    uint32_t r = rnd() % 100;
    if (r < 28) return strdup(common[rnd() % (sizeof common / sizeof common[0])]);
    int u = (*uid)++;
    switch (rnd() % 4) {
    case 0: return dupf("    int v%d = compute(%d, %d);", u, (int)(rnd() % 100), (int)(rnd() % 100));
    case 1: return dupf("    if (v%d > %d) {  /* case %d */", u, (int)(rnd() % 1000), u);
    case 2: return dupf("        total += step(%d, %d) * %d;", u, (int)(rnd() % 50), (int)(rnd() % 9));
    default: return dupf("    printf(\"item %d: %%d %d\\n\", v%d);", u, (int)(rnd() % 10), u);
    }
}

/* change the first number in a line, or return NULL if it has none */
static char *tweak_number(const char *s) {
    size_t n = strlen(s);
    size_t i = 0;
    while (i < n && !(s[i] >= '0' && s[i] <= '9')) i++;
    if (i == n) return NULL;
    size_t j = i;
    while (j < n && s[j] >= '0' && s[j] <= '9') j++;
    char *out = malloc(n + 16);
    memcpy(out, s, i);
    int k = snprintf(out + i, 16, "%u", rnd() % 100000);
    strcpy(out + i + k, s + j);
    return out;
}

static void write_lines(const char *path, char **v, int n) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "adiff gen: cannot write %s\n", path); exit(2); }
    for (int i = 0; i < n; i++) { fputs(v[i], f); fputc('\n', f); }
    fclose(f);
}

int cmd_gen(int argc, char **argv) {
    int lines = 2000, moves = 3;
    double rate = 0.05;
    uint64_t seed = 1;
    const char *files[2] = { NULL, NULL };
    int nf = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--lines") && i + 1 < argc) lines = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--edit-rate") && i + 1 < argc) rate = atof(argv[++i]);
        else if (!strcmp(argv[i], "--moves") && i + 1 < argc) moves = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint64_t)atoll(argv[++i]);
        else if (argv[i][0] == '-' && argv[i][1]) { fprintf(stderr, "adiff gen: unknown option '%s'\n", argv[i]); return 2; }
        else if (nf < 2) files[nf++] = argv[i];
    }
    if (nf != 2 || lines < 1) {
        fprintf(stderr, "usage: adiff gen [--lines N] [--edit-rate P] [--moves K] [--seed S] OLD NEW\n");
        return 2;
    }
    rng_seed(seed);
    int uid = 1;
    char **base = malloc(sizeof(char *) * (size_t)lines);
    for (int i = 0; i < lines; i++) base[i] = make_line(&uid);

    int cap = lines * 2 + 16, n = 0;
    char **nw = malloc(sizeof(char *) * (size_t)cap);
    for (int i = 0; i < lines; i++) {
        double r = rndf();
        if (r < rate / 3) {
            /* deleted */
        } else if (r < 2 * rate / 3) {
            char *t = tweak_number(base[i]);
            nw[n++] = t ? t : make_line(&uid);
        } else if (r < rate) {
            nw[n++] = strdup(base[i]);
            nw[n++] = make_line(&uid);
        } else {
            nw[n++] = strdup(base[i]);
        }
    }
    for (int k = 0; k < moves && n > 60; k++) {          /* relocate whole blocks */
        int len = 5 + (int)(rnd() % 21);
        int from = (int)(rnd() % (uint32_t)(n - len));
        char **blk = malloc(sizeof(char *) * (size_t)len);
        memcpy(blk, nw + from, sizeof(char *) * (size_t)len);
        memmove(nw + from, nw + from + len, sizeof(char *) * (size_t)(n - from - len));
        int rest = n - len;
        int to = (int)(rnd() % (uint32_t)(rest + 1));
        memmove(nw + to + len, nw + to, sizeof(char *) * (size_t)(rest - to));
        memcpy(nw + to, blk, sizeof(char *) * (size_t)len);
        free(blk);
    }
    write_lines(files[0], base, lines);
    write_lines(files[1], nw, n);
    printf("wrote %s (%d lines) and %s (%d lines), edit rate %.0f%%, %d moved blocks\n",
           files[0], lines, files[1], n, rate * 100.0, moves);
    return 0;
}

/* ------------------------------------------------------------------ */
/* selftest                                                            */
/* ------------------------------------------------------------------ */
static int check_all(const int *a, int m, const int *b, int n, int nids, const char *tag) {
    static const Engine engs[] = { ENG_CLASSIC, ENG_FULL, ENG_HIRSCH, ENG_BAND, ENG_MYERS, ENG_AUTO };
    int ref = -1, bad = 0;
    for (size_t e = 0; e < sizeof engs / sizeof engs[0]; e++) {
        MatchList ml;
        ml_init(&ml);
        EngineInfo info;
        int rc = compute_matches(a, m, b, n, nids, engs[e], &ml, &info);
        if (rc != 0) {
            printf("FAIL [%s] engine %s returned %d (m=%d n=%d)\n", tag, engine_name(engs[e]), rc, m, n);
            bad = 1;
            ml_free(&ml);
            continue;
        }
        int ok = 1, pa = -1, pb = -1;
        for (int t = 0; t < ml.n && ok; t++) {
            int x = ml.v[t].a, y = ml.v[t].b;
            if (x < 0 || x >= m || y < 0 || y >= n || x <= pa || y <= pb || a[x] != b[y]) ok = 0;
            pa = x; pb = y;
        }
        if (!ok) { printf("FAIL [%s] engine %s gave invalid matches (m=%d n=%d)\n", tag, engine_name(engs[e]), m, n); bad = 1; }
        if (e == 0) ref = ml.n;
        else if (ml.n != ref) {
            printf("FAIL [%s] engine %s found LCS %d, classic found %d (m=%d n=%d)\n", tag, engine_name(engs[e]), ml.n, ref, m, n);
            bad = 1;
        }
        ml_free(&ml);
    }
    return bad;
}

static int check_moves(const int *a, int m, const int *b, int n, int nids) {
    MatchList ml;
    ml_init(&ml);
    if (compute_matches(a, m, b, n, nids, ENG_AUTO, &ml, NULL) != 0) return 1;
    unsigned char *ma = calloc((size_t)m + 1, 1), *mb = calloc((size_t)n + 1, 1);
    unsigned char *triv = calloc((size_t)nids + 1, 1);
    unsigned char *ua = calloc((size_t)m + 1, 1), *ub = calloc((size_t)n + 1, 1);
    for (int t = 0; t < ml.n; t++) { ma[ml.v[t].a] = 1; mb[ml.v[t].b] = 1; }
    MoveList mv = { NULL, 0, 0 };
    detect_moves(a, m, b, n, triv, ma, mb, 3, &mv);
    int bad = 0;
    for (int t = 0; t < mv.n && !bad; t++) {
        for (int l = 0; l < mv.v[t].len; l++) {
            int x = mv.v[t].a + l, y = mv.v[t].b + l;
            if (x >= m || y >= n || a[x] != b[y] || ma[x] || mb[y] || ua[x] || ub[y]) { bad = 1; break; }
            ua[x] = ub[y] = 1;
        }
    }
    moves_free(&mv);
    ml_free(&ml);
    free(ma); free(mb); free(triv); free(ua); free(ub);
    return bad;
}

int cmd_selftest(int argc, char **argv) {
    (void)argc; (void)argv;
    rng_seed(12345);
    int fails = 0, tests = 0;

    /* 1. thousands of tiny random cases with small alphabets (many ties) */
    for (int it = 0; it < 3000; it++) {
        int alpha = 2 + (int)(rnd() % 6);
        int m = (int)(rnd() % 40), n = (int)(rnd() % 40);
        int *a = malloc(sizeof(int) * (size_t)(m + 1)), *b = malloc(sizeof(int) * (size_t)(n + 1));
        for (int i = 0; i < m; i++) a[i] = (int)(rnd() % (uint32_t)alpha);
        for (int j = 0; j < n; j++) b[j] = (int)(rnd() % (uint32_t)alpha);
        fails += check_all(a, m, b, n, alpha, "tiny");
        tests++;
        free(a); free(b);
    }

    /* 2. mutated copies of a longer sequence, with moved blocks */
    for (int it = 0; it < 60; it++) {
        int m = 300 + (int)(rnd() % 1500);
        int alpha = 40 + (int)(rnd() % 400);
        double rate = 0.01 + rndf() * 0.5;
        int *a = malloc(sizeof(int) * (size_t)m), *b = malloc(sizeof(int) * (size_t)(m * 2 + 8));
        for (int i = 0; i < m; i++) a[i] = (int)(rnd() % (uint32_t)alpha);
        int n = 0;
        for (int i = 0; i < m; i++) {
            double r = rndf();
            if (r < rate / 3) continue;
            if (r < 2 * rate / 3) b[n++] = (int)(rnd() % (uint32_t)alpha);
            else if (r < rate) { b[n++] = a[i]; b[n++] = (int)(rnd() % (uint32_t)alpha); }
            else b[n++] = a[i];
        }
        if (n > 80) {                       /* move one block */
            int len = 10 + (int)(rnd() % 30);
            int from = (int)(rnd() % (uint32_t)(n - len));
            int *blk = malloc(sizeof(int) * (size_t)len);
            memcpy(blk, b + from, sizeof(int) * (size_t)len);
            memmove(b + from, b + from + len, sizeof(int) * (size_t)(n - from - len));
            int rest = n - len, to = (int)(rnd() % (uint32_t)(rest + 1));
            memmove(b + to + len, b + to, sizeof(int) * (size_t)(rest - to));
            memcpy(b + to, blk, sizeof(int) * (size_t)len);
            free(blk);
        }
        fails += check_all(a, m, b, n, alpha, "mutated");
        fails += check_moves(a, m, b, n, alpha);
        tests++;
        free(a); free(b);
    }

    /* 3. large enough to leave the "small input" path of the selector:
     *    ~2% edits uses the banded engine, ~40% edits falls back to Hirschberg */
    for (int it = 0; it < 4; it++) {
        int m = 2600;
        int alpha = 900;
        double rate = it < 2 ? 0.02 : 0.40;
        int *a = malloc(sizeof(int) * (size_t)m), *b = malloc(sizeof(int) * (size_t)(m * 2));
        for (int i = 0; i < m; i++) a[i] = (int)(rnd() % (uint32_t)alpha);
        int n = 0;
        for (int i = 0; i < m; i++) {
            double r = rndf();
            if (r < rate / 2) continue;
            if (r < rate) b[n++] = (int)(rnd() % (uint32_t)alpha);
            else b[n++] = a[i];
        }
        MatchList ml;
        ml_init(&ml);
        EngineInfo info;
        if (compute_matches(a, m, b, n, alpha, ENG_AUTO, &ml, &info) == 0)
            printf("  selector on %d x %d lines with %.0f%% edits chose: %s\n", m, n, rate * 100, engine_name(info.used));
        ml_free(&ml);
        fails += check_all(a, m, b, n, alpha, "large");
        tests++;
        free(a); free(b);
    }

    printf("selftest: %d test groups, %d failures\n", tests, fails);
    return fails ? 1 : 0;
}
