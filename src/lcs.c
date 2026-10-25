/*
 * lcs.c - the LCS engines and the adaptive selector.
 *
 *   full table   : O(mn) time, O(mn) space  (textbook, CLRS)
 *   Hirschberg   : O(mn) time, O(min(m,n)) space (divide and conquer)
 *   banded DP    : O(kn) time and space, exact thanks to a doubling band
 *   Myers        : O(ND) time (baseline, the algorithm behind git diff)
 *
 * Every engine returns an OPTIMAL common subsequence, so the selector only
 * changes the cost of the computation, never the answer.
 */
#include "adiff.h"
#include <stdlib.h>
#include <string.h>

/* limits (tuned in Phase 2 experiments) */
#define FULL_CELL_LIMIT   ((size_t)400000000)      /* 1.6 GB of int cells   */
#define AUTO_FULL_CELLS   ((double)4000000.0)      /* selector: small input */
#define AUTO_BAND_PERCENT 10                       /* selector: similar     */
#define BAND_START_K      64
#define BAND_MEM_LIMIT    ((size_t)256 << 20)
#define MYERS_MEM_LIMIT   ((size_t)512 << 20)

/* ------------------------------------------------------------------ */
/* MatchList                                                           */
/* ------------------------------------------------------------------ */
void ml_init(MatchList *l) { l->v = NULL; l->n = l->cap = 0; }

void ml_push(MatchList *l, int a, int b) {
    if (l->n == l->cap) {
        int c = l->cap ? l->cap * 2 : 64;
        Match *t = realloc(l->v, (size_t)c * sizeof(Match));
        if (!t) { fprintf(stderr, "adiff: out of memory\n"); exit(2); }
        l->v = t;
        l->cap = c;
    }
    l->v[l->n].a = a;
    l->v[l->n].b = b;
    l->n++;
}

void ml_free(MatchList *l) { free(l->v); ml_init(l); }

/* ------------------------------------------------------------------ */
/* 1. Full table                                                       */
/* ------------------------------------------------------------------ */
static int lcs_full(const int *a, int m, const int *b, int n, MatchList *out) {
    size_t w = (size_t)n + 1;
    size_t cells = (size_t)(m + 1) * w;
    if (cells > FULL_CELL_LIMIT) return -1;
    int *L = calloc(cells, sizeof(int));
    if (!L) return -1;
    for (int i = 1; i <= m; i++) {
        for (int j = 1; j <= n; j++) {
            if (a[i - 1] == b[j - 1]) {
                L[i * w + j] = L[(i - 1) * w + (j - 1)] + 1;
            } else {
                int up = L[(i - 1) * w + j], left = L[i * w + (j - 1)];
                L[i * w + j] = up > left ? up : left;
            }
        }
    }
    int c = L[(size_t)m * w + (size_t)n];
    Match *tmp = malloc(sizeof(Match) * (size_t)(c > 0 ? c : 1));
    int i = m, j = n, k = c;
    while (i > 0 && j > 0) {
        if (a[i - 1] == b[j - 1]) {
            tmp[--k].a = i - 1;
            tmp[k].b = j - 1;
            i--; j--;
        } else if (L[(size_t)(i - 1) * w + j] >= L[(size_t)i * w + (j - 1)]) {
            i--;
        } else {
            j--;
        }
    }
    for (int t = 0; t < c; t++) ml_push(out, tmp[t].a, tmp[t].b);
    free(tmp);
    free(L);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 2. Hirschberg                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    const int *a, *b;
    int *f, *g;
    MatchList *out;
    int swap;
} HCtx;

/* r[j] = LCS(a[0..al), b[0..j)) for j = 0..bl, using ONE row */
static void row_fwd(const int *a, int al, const int *b, int bl, int *r) {
    for (int j = 0; j <= bl; j++) r[j] = 0;
    for (int i = 0; i < al; i++) {
        int diag = 0;
        for (int j = 1; j <= bl; j++) {
            int up = r[j];
            if (a[i] == b[j - 1]) r[j] = diag + 1;
            else if (r[j - 1] > r[j]) r[j] = r[j - 1];
            diag = up;
        }
    }
}

/* g[j] = LCS(a[0..al), b[j..bl)) for j = 0..bl, using ONE row */
static void row_bwd(const int *a, int al, const int *b, int bl, int *g) {
    for (int j = 0; j <= bl; j++) g[j] = 0;
    for (int i = al - 1; i >= 0; i--) {
        int diag = 0;
        for (int j = bl - 1; j >= 0; j--) {
            int up = g[j];
            if (a[i] == b[j]) g[j] = diag + 1;
            else if (g[j + 1] > g[j]) g[j] = g[j + 1];
            diag = up;
        }
    }
}

static void hirsch(HCtx *c, int alo, int ahi, int blo, int bhi) {
    int m = ahi - alo, n = bhi - blo;
    if (m <= 0 || n <= 0) return;
    if (m == 1) {
        for (int j = blo; j < bhi; j++) {
            if (c->a[alo] == c->b[j]) {
                if (c->swap) ml_push(c->out, j, alo);
                else ml_push(c->out, alo, j);
                return;
            }
        }
        return;
    }
    int mid = alo + m / 2;
    row_fwd(c->a + alo, mid - alo, c->b + blo, n, c->f);
    row_bwd(c->a + mid, ahi - mid, c->b + blo, n, c->g);
    int best = -1, k = 0;
    for (int t = 0; t <= n; t++) {
        int s = c->f[t] + c->g[t];
        if (s > best) { best = s; k = t; }
    }
    hirsch(c, alo, mid, blo, blo + k);
    hirsch(c, mid, ahi, blo + k, bhi);
}

static int lcs_hirsch(const int *a, int m, const int *b, int n, MatchList *out) {
    HCtx c;
    c.out = out;
    /* keep the SHORTER sequence as the row dimension: O(min(m,n)) space */
    if (n <= m) { c.a = a; c.b = b; c.swap = 0; }
    else        { c.a = b; c.b = a; c.swap = 1; }
    int mm = c.swap ? n : m, nn = c.swap ? m : n;
    c.f = malloc(sizeof(int) * (size_t)(nn + 1));
    c.g = malloc(sizeof(int) * (size_t)(nn + 1));
    if (!c.f || !c.g) { free(c.f); free(c.g); return -1; }
    hirsch(&c, 0, mm, 0, nn);
    free(c.f);
    free(c.g);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 3. Banded DP with a doubling band                                   */
/* ------------------------------------------------------------------ */
#define INF (1 << 29)

typedef struct { int *D; int m, n, dlo, dhi, w; } Band;

static inline int bget(const Band *B, int i, int j) {
    if (i < 0 || j < 0 || j > B->n) return INF;
    int d = j - i;
    if (d < B->dlo || d > B->dhi) return INF;
    return B->D[(size_t)i * (size_t)B->w + (size_t)(d - B->dlo)];
}

/* D[i][j] = insert/delete distance of a[0..i), b[0..j) restricted to the
 * diagonals dlo..dhi. If D[m][n] <= k the answer is provably optimal, since
 * any path that leaves the band already costs more than k. */
static int band_try(const int *a, int m, const int *b, int n, int k,
                    size_t memlimit, MatchList *out, int *found) {
    *found = 0;
    int delta = n - m, ad = delta < 0 ? -delta : delta;
    if (k < ad) k = ad;
    int slack = (k - ad) / 2;
    int dlo = (delta < 0 ? delta : 0) - slack;
    int dhi = (delta > 0 ? delta : 0) + slack;
    size_t w = (size_t)(dhi - dlo + 1);
    size_t cells = (size_t)(m + 1) * w;
    if (cells * sizeof(int) > memlimit) return -1;
    int *D = malloc(cells * sizeof(int));
    if (!D) return -1;
    for (size_t t = 0; t < cells; t++) D[t] = INF;
    Band B = { D, m, n, dlo, dhi, (int)w };

    for (int i = 0; i <= m; i++) {
        int jlo = i + dlo; if (jlo < 0) jlo = 0;
        int jhi = i + dhi; if (jhi > n) jhi = n;
        for (int j = jlo; j <= jhi; j++) {
            int v;
            if (i == 0 && j == 0) {
                v = 0;
            } else {
                v = INF;
                int t = bget(&B, i - 1, j) + 1;
                if (t < v) v = t;
                t = bget(&B, i, j - 1) + 1;
                if (t < v) v = t;
                if (i > 0 && j > 0 && a[i - 1] == b[j - 1]) {
                    t = bget(&B, i - 1, j - 1);
                    if (t < v) v = t;
                }
            }
            D[(size_t)i * w + (size_t)(j - i - dlo)] = v;
        }
    }

    int dist = bget(&B, m, n);
    if (dist <= k) {
        int c = (m + n - dist) / 2;
        Match *tmp = malloc(sizeof(Match) * (size_t)(c > 0 ? c : 1));
        int i = m, j = n, idx = c;
        while (i > 0 || j > 0) {
            int cur = bget(&B, i, j);
            if (i > 0 && j > 0 && a[i - 1] == b[j - 1] && bget(&B, i - 1, j - 1) == cur) {
                tmp[--idx].a = i - 1;
                tmp[idx].b = j - 1;
                i--; j--;
            } else if (i > 0 && bget(&B, i - 1, j) + 1 == cur) {
                i--;
            } else {
                j--;
            }
        }
        for (int t = 0; t < c; t++) ml_push(out, tmp[t].a, tmp[t].b);
        free(tmp);
        *found = 1;
    }
    free(D);
    return 0;
}

/* 0 = ok, -1 = memory limit, -2 = no answer within kmax */
static int lcs_banded(const int *a, int m, const int *b, int n, int kmax,
                      size_t memlimit, MatchList *out, int *kused) {
    int ad = abs(n - m);
    if (kmax < ad) return -2;
    int k = BAND_START_K;
    if (k < ad) k = ad;
    if (k > kmax) k = kmax;
    for (;;) {
        int found = 0;
        int rc = band_try(a, m, b, n, k, memlimit, out, &found);
        if (rc < 0) return -1;
        if (found) { *kused = k; return 0; }
        if (k >= kmax) return -2;
        k = (k > kmax / 2) ? kmax : k * 2;
    }
}

/* ------------------------------------------------------------------ */
/* 4. Myers O(ND)                                                      */
/* ------------------------------------------------------------------ */
static int lcs_myers(const int *a, int m, const int *b, int n, MatchList *out) {
    int max = m + n;
    if (max == 0) return 0;
    int off = max + 1;
    int *V = calloc((size_t)(2 * max + 3), sizeof(int));
    int **trace = calloc((size_t)(max + 1), sizeof(int *));
    if (!V || !trace) { free(V); free(trace); return -1; }
    size_t used = 0;
    int D = -1;

    V[off + 1] = 0;
    for (int d = 0; d <= max && D < 0; d++) {
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && V[off + k - 1] < V[off + k + 1])) x = V[off + k + 1];
            else x = V[off + k - 1] + 1;
            int y = x - k;
            while (x < m && y < n && a[x] == b[y]) { x++; y++; }
            V[off + k] = x;
            if (x >= m && y >= n) { D = d; break; }
        }
        if (D >= 0) break;
        used += (size_t)(2 * d + 1) * sizeof(int);
        if (used > MYERS_MEM_LIMIT) {
            for (int t = 0; t < d; t++) free(trace[t]);
            free(trace); free(V);
            return -1;
        }
        trace[d] = malloc((size_t)(2 * d + 1) * sizeof(int));
        if (!trace[d]) {
            for (int t = 0; t < d; t++) free(trace[t]);
            free(trace); free(V);
            return -1;
        }
        memcpy(trace[d], V + off - d, (size_t)(2 * d + 1) * sizeof(int));
    }

    /* walk back from (m,n) collecting the diagonal runs (the matches) */
    Match *tmp = malloc(sizeof(Match) * (size_t)((m < n ? m : n) + 1));
    int cnt = 0, x = m, y = n;
    for (int d = D; d > 0; d--) {
        int k = x - y;
        const int *Vp = trace[d - 1];
        int pk;
        if (k == -d || (k != d && Vp[(k - 1) + (d - 1)] < Vp[(k + 1) + (d - 1)])) pk = k + 1;
        else pk = k - 1;
        int px = Vp[pk + (d - 1)];
        int py = px - pk;
        int sx = (pk == k + 1) ? px : px + 1;
        int sy = (pk == k + 1) ? py + 1 : py;
        while (x > sx && y > sy) {
            x--; y--;
            tmp[cnt].a = x;
            tmp[cnt].b = y;
            cnt++;
        }
        x = px;
        y = py;
    }
    while (x > 0 && y > 0) {
        x--; y--;
        tmp[cnt].a = x;
        tmp[cnt].b = y;
        cnt++;
    }
    for (int t = cnt - 1; t >= 0; t--) ml_push(out, tmp[t].a, tmp[t].b);

    free(tmp);
    for (int t = 0; t < D; t++) free(trace[t]);
    free(trace);
    free(V);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Selector and dispatcher                                             */
/* ------------------------------------------------------------------ */
static const char *names[] = { "auto", "classic", "full", "hirschberg", "banded", "myers" };

const char *engine_name(Engine e) { return names[e]; }

int engine_parse(const char *s, Engine *e) {
    for (int i = 0; i < 6; i++) {
        if (strcmp(s, names[i]) == 0) { *e = (Engine)i; return 0; }
    }
    return -1;
}

/* cheap lower bound on the edit distance from line frequencies:
 * D >= m + n - 2 * (number of lines the two sides have in common) */
static long distance_lower_bound(const int *a, int m, const int *b, int n, int nids) {
    int *cnt = calloc((size_t)nids + 1, sizeof(int));
    if (!cnt) return 0;
    for (int i = 0; i < m; i++) cnt[a[i]]++;
    long common = 0;
    for (int j = 0; j < n; j++) {
        if (cnt[b[j]] > 0) { cnt[b[j]]--; common++; }
    }
    free(cnt);
    return (long)m + n - 2 * common;
}

int compute_matches(const int *a, int m, const int *b, int n, int nids,
                    Engine eng, MatchList *out, EngineInfo *info) {
    EngineInfo local;
    if (!info) info = &local;
    memset(info, 0, sizeof *info);

    /* Step 2 of the pipeline: trim the identical head and tail */
    int p = 0, s = 0;
    if (eng != ENG_CLASSIC) {
        while (p < m && p < n && a[p] == b[p]) p++;
        while (s < m - p && s < n - p && a[m - 1 - s] == b[n - 1 - s]) s++;
    }
    info->trim_prefix = p;
    info->trim_suffix = s;
    for (int i = 0; i < p; i++) ml_push(out, i, i);

    int cm = m - p - s, cn = n - p - s;
    info->core_m = cm;
    info->core_n = cn;
    Engine use = eng;

    if (cm > 0 && cn > 0) {
        const int *ca = a + p, *cb = b + p;
        MatchList tmp;
        ml_init(&tmp);
        int rc = 0, done = 0;

        /* Step 3: the adaptive selector */
        if (use == ENG_AUTO) {
            double cells = (double)(cm + 1) * (double)(cn + 1);
            if (cells <= AUTO_FULL_CELLS) {
                use = ENG_FULL;
            } else {
                int big = cm > cn ? cm : cn;
                int cap = (int)((long long)big * AUTO_BAND_PERCENT / 100);
                use = ENG_HIRSCH;
                if (distance_lower_bound(ca, cm, cb, cn, nids) <= cap) {
                    int ku = 0;
                    int r2 = lcs_banded(ca, cm, cb, cn, cap, BAND_MEM_LIMIT, &tmp, &ku);
                    if (r2 == 0) { use = ENG_BAND; info->band_k = ku; done = 1; }
                    else tmp.n = 0;   /* band too narrow or too big: fall back */
                }
            }
        }

        /* Step 4: run the chosen engine */
        if (!done) {
            int ku = 0;
            switch (use) {
            case ENG_CLASSIC:
            case ENG_FULL:  rc = lcs_full(ca, cm, cb, cn, &tmp); break;
            case ENG_HIRSCH: rc = lcs_hirsch(ca, cm, cb, cn, &tmp); break;
            case ENG_BAND:  rc = lcs_banded(ca, cm, cb, cn, cm + cn, BAND_MEM_LIMIT, &tmp, &ku);
                            info->band_k = ku; break;
            case ENG_MYERS: rc = lcs_myers(ca, cm, cb, cn, &tmp); break;
            default: rc = -1; break;
            }
        }
        if (rc != 0) { ml_free(&tmp); info->used = use; return -1; }
        for (int t = 0; t < tmp.n; t++) ml_push(out, tmp.v[t].a + p, tmp.v[t].b + p);
        ml_free(&tmp);
    } else if (use == ENG_AUTO) {
        use = ENG_FULL;   /* nothing left to compare after trimming */
    }

    for (int t = 0; t < s; t++) ml_push(out, m - s + t, n - s + t);
    info->used = use;
    return 0;
}
