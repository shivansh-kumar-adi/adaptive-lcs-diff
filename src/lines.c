/*
 * lines.c - read two files and turn every line into an integer id.
 *
 * Equal lines get the same id through an exact (collision-free) intern
 * table, so comparing two lines later costs O(1).
 */
#include "adiff.h"
#include <stdlib.h>
#include <string.h>

static int read_file(const char *path, char **out, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t cap = 1 << 16, n = 0;
    char *b = malloc(cap);
    if (!b) { fclose(f); return -1; }
    for (;;) {
        if (n == cap) {
            cap *= 2;
            char *t = realloc(b, cap);
            if (!t) { free(b); fclose(f); return -1; }
            b = t;
        }
        size_t r = fread(b + n, 1, cap - n, f);
        n += r;
        if (r == 0) break;
    }
    fclose(f);
    *out = b;
    *len = n;
    return 0;
}

static uint64_t fnv(const char *s, int n) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

typedef struct {
    int *tab;
    size_t mask;
    const char **kp;
    int *kl;
    uint64_t *kh;
    unsigned char *triv;
    int n;
} Intern;

static int intern_get(Intern *t, const char *s, int len) {
    uint64_t h = fnv(s, len);
    size_t slot = (size_t)h & t->mask;
    while (t->tab[slot] != -1) {
        int e = t->tab[slot];
        if (t->kh[e] == h && t->kl[e] == len && memcmp(t->kp[e], s, (size_t)len) == 0)
            return e;
        slot = (slot + 1) & t->mask;
    }
    int e = t->n++;
    t->kp[e] = s;
    t->kl[e] = len;
    t->kh[e] = h;
    int sig = 0;
    for (int i = 0; i < len; i++)
        if (s[i] != ' ' && s[i] != '\t') sig++;
    t->triv[e] = sig < 3;      /* blank lines, "}", "{", "end" ... */
    t->tab[slot] = e;
    return e;
}

static int count_lines(const char *buf, size_t sz) {
    int n = 0;
    for (size_t i = 0; i < sz; i++)
        if (buf[i] == '\n') n++;
    if (sz > 0 && buf[sz - 1] != '\n') n++;
    return n;
}

static void split_lines(Lines *L, char *buf, size_t sz, int n, Intern *t) {
    L->buf = buf;
    L->n = n;
    L->ptr = malloc(sizeof(char *) * (size_t)(n + 1));
    L->len = malloc(sizeof(int) * (size_t)(n + 1));
    L->id = malloc(sizeof(int) * (size_t)(n + 1));
    size_t pos = 0;
    for (int k = 0; k < n; k++) {
        size_t st = pos;
        while (pos < sz && buf[pos] != '\n') pos++;
        size_t en = pos;
        if (pos < sz) pos++;
        if (en > st && buf[en - 1] == '\r') en--;   /* CRLF == LF */
        L->ptr[k] = buf + st;
        L->len[k] = (int)(en - st);
        L->id[k] = intern_get(t, L->ptr[k], L->len[k]);
    }
}

int pair_load(Pair *p, const char *old_path, const char *new_path) {
    memset(p, 0, sizeof *p);
    char *ba = NULL, *bb = NULL;
    size_t sa = 0, sb = 0;
    if (read_file(old_path, &ba, &sa) != 0) {
        fprintf(stderr, "adiff: cannot read '%s'\n", old_path);
        return -1;
    }
    if (read_file(new_path, &bb, &sb) != 0) {
        fprintf(stderr, "adiff: cannot read '%s'\n", new_path);
        free(ba);
        return -1;
    }
    int na = count_lines(ba, sa), nb = count_lines(bb, sb);
    size_t total = (size_t)na + (size_t)nb + 1;
    size_t cap = 16;
    while (cap < total * 2) cap <<= 1;

    Intern t;
    t.tab = malloc(cap * sizeof(int));
    for (size_t i = 0; i < cap; i++) t.tab[i] = -1;
    t.mask = cap - 1;
    t.kp = malloc(total * sizeof(char *));
    t.kl = malloc(total * sizeof(int));
    t.kh = malloc(total * sizeof(uint64_t));
    t.triv = malloc(total);
    t.n = 0;

    split_lines(&p->a, ba, sa, na, &t);
    split_lines(&p->b, bb, sb, nb, &t);
    p->nids = t.n;
    p->trivial = t.triv;
    free(t.tab);
    free(t.kp);
    free(t.kl);
    free(t.kh);
    return 0;
}

void pair_free(Pair *p) {
    free(p->a.buf); free(p->a.ptr); free(p->a.len); free(p->a.id);
    free(p->b.buf); free(p->b.ptr); free(p->b.len); free(p->b.id);
    free(p->trivial);
    memset(p, 0, sizeof *p);
}
