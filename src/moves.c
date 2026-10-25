/*
 * moves.c - find blocks of lines that were moved, not rewritten.
 *
 * After the LCS, every line that is not matched is a deletion (old file) or
 * an insertion (new file). A block of at least `min_len` unmatched lines that
 * appears on both sides is reported as ONE move instead of two edits.
 *
 *  1. hash every window of K consecutive deleted lines into a hash map
 *  2. look up every window of K consecutive inserted lines
 *  3. extend the match to the maximal block and verify it line by line
 *     (so a hash collision can never create a false move)
 *  4. accept the largest blocks first; each line is used at most once
 */
#include "adiff.h"
#include <stdlib.h>
#include <string.h>

#define CHAIN_LIMIT 32
#define MIN_ANCHORS 2   /* a block needs at least 2 non-trivial lines */

typedef struct { int i, j, len; } Cand;

static uint64_t mix(uint64_t h, uint64_t x) {
    h ^= x + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdULL;
}

static uint64_t window_hash(const int *a, int i, int k) {
    uint64_t h = 1469598103934665603ULL;
    for (int t = 0; t < k; t++) h = mix(h, (uint64_t)(unsigned)a[i + t]);
    return h;
}

static int cand_cmp(const void *x, const void *y) {
    const Cand *p = x, *q = y;
    if (p->len != q->len) return q->len - p->len;               /* longest first */
    int dp = abs(p->i - p->j), dq = abs(q->i - q->j);
    if (dp != dq) return dp - dq;                                /* nearest first */
    if (p->i != q->i) return p->i - q->i;
    return p->j - q->j;
}

static void move_push(MoveList *l, int a, int b, int len) {
    if (l->n == l->cap) {
        int c = l->cap ? l->cap * 2 : 16;
        Move *t = realloc(l->v, (size_t)c * sizeof(Move));
        if (!t) { fprintf(stderr, "adiff: out of memory\n"); exit(2); }
        l->v = t;
        l->cap = c;
    }
    l->v[l->n].a = a;
    l->v[l->n].b = b;
    l->v[l->n].len = len;
    l->n++;
}

void moves_free(MoveList *l) { free(l->v); l->v = NULL; l->n = l->cap = 0; }

void detect_moves(const int *a, int m, const int *b, int n,
                  const unsigned char *trivial,
                  const unsigned char *matched_a, const unsigned char *matched_b,
                  int K, MoveList *out) {
    out->n = 0;
    if (K < 2) K = 2;
    if (m < K || n < K) return;

    /* length of the unmatched run starting at each position */
    int *runA = malloc(sizeof(int) * (size_t)m);
    int *runB = malloc(sizeof(int) * (size_t)n);
    for (int i = m - 1; i >= 0; i--)
        runA[i] = matched_a[i] ? 0 : (i + 1 < m ? runA[i + 1] : 0) + 1;
    for (int j = n - 1; j >= 0; j--)
        runB[j] = matched_b[j] ? 0 : (j + 1 < n ? runB[j + 1] : 0) + 1;

    size_t HB = 16;
    while (HB < (size_t)m * 2) HB <<= 1;
    int *head = malloc(HB * sizeof(int));
    for (size_t t = 0; t < HB; t++) head[t] = -1;
    int *nxt = malloc(sizeof(int) * (size_t)m);
    uint64_t *wh = malloc(sizeof(uint64_t) * (size_t)m);

    for (int i = 0; i + K <= m; i++) {
        if (runA[i] < K) continue;
        uint64_t h = window_hash(a, i, K);
        wh[i] = h;
        size_t bk = (size_t)h & (HB - 1);
        nxt[i] = head[bk];
        head[bk] = i;
    }

    Cand *cand = NULL;
    int nc = 0, cc = 0;
    for (int j = 0; j + K <= n; j++) {
        if (runB[j] < K) continue;
        uint64_t h = window_hash(b, j, K);
        int steps = 0;
        for (int i = head[(size_t)h & (HB - 1)]; i >= 0 && steps < CHAIN_LIMIT; i = nxt[i], steps++) {
            if (wh[i] != h) continue;
            int same = 1;
            for (int t = 0; t < K; t++)
                if (a[i + t] != b[j + t]) { same = 0; break; }
            if (!same) continue;
            /* only start from the first line of a block */
            if (i > 0 && j > 0 && a[i - 1] == b[j - 1] && !matched_a[i - 1] && !matched_b[j - 1])
                continue;
            int len = K;
            while (i + len < m && j + len < n && a[i + len] == b[j + len] &&
                   !matched_a[i + len] && !matched_b[j + len])
                len++;
            int anchors = 0;
            for (int t = 0; t < len; t++)
                if (!trivial[a[i + t]]) anchors++;
            if (anchors < MIN_ANCHORS) continue;
            if (nc == cc) {
                cc = cc ? cc * 2 : 64;
                Cand *t2 = realloc(cand, (size_t)cc * sizeof(Cand));
                if (!t2) { fprintf(stderr, "adiff: out of memory\n"); exit(2); }
                cand = t2;
            }
            cand[nc].i = i;
            cand[nc].j = j;
            cand[nc].len = len;
            nc++;
        }
    }

    if (nc > 0) {
        qsort(cand, (size_t)nc, sizeof(Cand), cand_cmp);
        unsigned char *usedA = calloc((size_t)m, 1);
        unsigned char *usedB = calloc((size_t)n, 1);
        for (int c = 0; c < nc; c++) {
            int free_ok = 1;
            for (int t = 0; t < cand[c].len; t++) {
                if (usedA[cand[c].i + t] || usedB[cand[c].j + t]) { free_ok = 0; break; }
            }
            if (!free_ok) continue;
            for (int t = 0; t < cand[c].len; t++) {
                usedA[cand[c].i + t] = 1;
                usedB[cand[c].j + t] = 1;
            }
            move_push(out, cand[c].i, cand[c].j, cand[c].len);
        }
        free(usedA);
        free(usedB);
    }

    free(cand);
    free(runA);
    free(runB);
    free(head);
    free(nxt);
    free(wh);
}
