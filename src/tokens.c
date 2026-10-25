/*
 * tokens.c - token level diff inside a changed line.
 *
 * A tiny lexer splits a line into whitespace, identifiers, numbers, string
 * literals and single punctuation characters. A second LCS (on the tokens,
 * ignoring whitespace) finds which tokens really changed, so only those are
 * marked instead of the whole line.
 */
#include "adiff.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOKENS 1500   /* above this a line is too long for the token DP */

/* 0 whitespace, 1 identifier, 2 number, 3 string literal, 4 other */
static int kind_of(char ch) {
    unsigned char c = (unsigned char)ch;
    if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') return 0;
    if (isalpha(c) || c == '_' || c >= 128) return 1;
    if (isdigit(c)) return 2;
    if (c == '"' || c == '\'') return 3;
    return 4;
}

int tokenize(const char *s, int len, Tok **out) {
    int cap = 16, n = 0, i = 0;
    Tok *t = malloc((size_t)cap * sizeof(Tok));
    while (i < len) {
        int st = i, k = kind_of(s[i]);
        if (k == 0) {
            while (i < len && kind_of(s[i]) == 0) i++;
        } else if (k == 1) {
            while (i < len && (kind_of(s[i]) == 1 || kind_of(s[i]) == 2)) i++;
        } else if (k == 2) {
            while (i < len && (isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '.')) i++;
        } else if (k == 3) {
            char q = s[i++];
            while (i < len && s[i] != q) {
                if (s[i] == '\\' && i + 1 < len) i++;
                i++;
            }
            if (i < len) i++;
        } else {
            i++;
        }
        if (n == cap) {
            cap *= 2;
            t = realloc(t, (size_t)cap * sizeof(Tok));
        }
        t[n].s = s + st;
        t[n].len = i - st;
        t[n].kind = k;
        n++;
    }
    *out = t;
    return n;
}

static int tok_eq(const Tok *x, const Tok *y) {
    return x->len == y->len && memcmp(x->s, y->s, (size_t)x->len) == 0;
}

/* whitespace between two changed tokens belongs to the changed run */
static void fill_gaps(const Tok *t, int n, unsigned char *f) {
    for (int k = 1; k + 1 < n; k++)
        if (t[k].kind == 0 && f[k - 1] && f[k + 1]) f[k] = 1;
}

int token_pair(const char *sa, int la, const char *sb, int lb,
               Tok **ta, int *na, unsigned char **fa,
               Tok **tb, int *nb, unsigned char **fb) {
    *ta = *tb = NULL;
    *fa = *fb = NULL;
    *na = *nb = 0;

    Tok *xa, *xb;
    int cna = tokenize(sa, la, &xa);
    int cnb = tokenize(sb, lb, &xb);
    int *ia = malloc(sizeof(int) * (size_t)(cna + 1));
    int *ib = malloc(sizeof(int) * (size_t)(cnb + 1));
    int p = 0, q = 0;
    for (int k = 0; k < cna; k++) if (xa[k].kind != 0) ia[p++] = k;
    for (int k = 0; k < cnb; k++) if (xb[k].kind != 0) ib[q++] = k;

    int ok = 0;
    if (p + q > 0 && p <= MAX_TOKENS && q <= MAX_TOKENS) {
        size_t w = (size_t)q + 1;
        int *L = calloc((size_t)(p + 1) * w, sizeof(int));
        unsigned char *ma = calloc((size_t)p + 1, 1);
        unsigned char *mb = calloc((size_t)q + 1, 1);
        if (L && ma && mb) {
            for (int i = 1; i <= p; i++)
                for (int j = 1; j <= q; j++) {
                    if (tok_eq(&xa[ia[i - 1]], &xb[ib[j - 1]])) L[i * w + j] = L[(i - 1) * w + j - 1] + 1;
                    else {
                        int up = L[(i - 1) * w + j], lf = L[i * w + j - 1];
                        L[i * w + j] = up > lf ? up : lf;
                    }
                }
            int lcs = L[(size_t)p * w + (size_t)q];
            if (4 * lcs >= p + q) {            /* similarity >= 50% */
                int i = p, j = q;
                while (i > 0 && j > 0) {
                    if (tok_eq(&xa[ia[i - 1]], &xb[ib[j - 1]])) { ma[i - 1] = mb[j - 1] = 1; i--; j--; }
                    else if (L[(size_t)(i - 1) * w + j] >= L[(size_t)i * w + j - 1]) i--;
                    else j--;
                }
                unsigned char *f1 = calloc((size_t)cna + 1, 1);
                unsigned char *f2 = calloc((size_t)cnb + 1, 1);
                for (int k = 0; k < p; k++) if (!ma[k]) f1[ia[k]] = 1;
                for (int k = 0; k < q; k++) if (!mb[k]) f2[ib[k]] = 1;
                fill_gaps(xa, cna, f1);
                fill_gaps(xb, cnb, f2);
                *ta = xa; *na = cna; *fa = f1;
                *tb = xb; *nb = cnb; *fb = f2;
                ok = 1;
            }
        }
        free(L); free(ma); free(mb);
    }
    free(ia);
    free(ib);
    if (!ok) { free(xa); free(xb); }
    return ok;
}
