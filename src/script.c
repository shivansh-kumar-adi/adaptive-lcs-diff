/*
 * script.c - turn matches into an edit script, annotate it with moves and
 * token level marks, print it, and verify it.
 */
#include "adiff.h"
#include <stdlib.h>
#include <string.h>

int build_ops(const Pair *p, const MatchList *ml, OpList *ops) {
    int m = p->a.n, n = p->b.n;
    ops->v = calloc((size_t)(m + n + 1), sizeof(Op));
    ops->n = 0;
    if (!ops->v) return -1;
    int i = 0, j = 0;
    for (int t = 0; t <= ml->n; t++) {
        int ma = t < ml->n ? ml->v[t].a : m;
        int mb = t < ml->n ? ml->v[t].b : n;
        while (i < ma) { Op *o = &ops->v[ops->n++]; o->t = OP_DEL; o->a = i++; o->b = -1; }
        while (j < mb) { Op *o = &ops->v[ops->n++]; o->t = OP_INS; o->a = -1; o->b = j++; }
        if (t < ml->n) {
            Op *o = &ops->v[ops->n++];
            o->t = OP_EQ; o->a = i++; o->b = j++;
        }
    }
    return 0;
}

void ops_free(OpList *ops) {
    for (int i = 0; i < ops->n; i++) { free(ops->v[i].tk); free(ops->v[i].tf); }
    free(ops->v);
    ops->v = NULL;
    ops->n = 0;
}

int ops_have_changes(const OpList *ops) {
    for (int i = 0; i < ops->n; i++) if (ops->v[i].t != OP_EQ) return 1;
    return 0;
}

void annotate_moves(OpList *ops, const MoveList *mv, int m, int n) {
    int *ma = calloc((size_t)m + 1, sizeof(int));
    int *mb = calloc((size_t)n + 1, sizeof(int));
    for (int t = 0; t < mv->n; t++)
        for (int l = 0; l < mv->v[t].len; l++) {
            ma[mv->v[t].a + l] = t + 1;
            mb[mv->v[t].b + l] = t + 1;
        }
    for (int i = 0; i < ops->n; i++) {
        Op *o = &ops->v[i];
        if (o->t == OP_DEL) o->mv = ma[o->a];
        else if (o->t == OP_INS) o->mv = mb[o->b];
    }
    free(ma);
    free(mb);
}

void annotate_tokens(const Pair *p, OpList *ops) {
    int n = ops->n, i = 0;
    int *dl = malloc(sizeof(int) * (size_t)(n + 1));
    int *il = malloc(sizeof(int) * (size_t)(n + 1));
    while (i < n) {
        if (ops->v[i].t == OP_EQ) { i++; continue; }
        int nd = 0, ni = 0;
        while (i < n && ops->v[i].t != OP_EQ) {
            if (!ops->v[i].mv) {
                if (ops->v[i].t == OP_DEL) dl[nd++] = i;
                else il[ni++] = i;
            }
            i++;
        }
        int k = nd < ni ? nd : ni;      /* pair the t-th deletion with the t-th insertion */
        for (int t = 0; t < k; t++) {
            Op *d = &ops->v[dl[t]], *a = &ops->v[il[t]];
            token_pair(p->a.ptr[d->a], p->a.len[d->a], p->b.ptr[a->b], p->b.len[a->b],
                       &d->tk, &d->ntk, &d->tf, &a->tk, &a->ntk, &a->tf);
        }
    }
    free(dl);
    free(il);
}

/* ------------------------------------------------------------------ */
/* printing                                                            */
/* ------------------------------------------------------------------ */
#define C_RED  "\033[31m"
#define C_GRN  "\033[32m"
#define C_CYN  "\033[36m"
#define C_MAG  "\033[35m"
#define C_RST  "\033[0m"

static void print_text(FILE *o, const char *s, int len) { fwrite(s, 1, (size_t)len, o); }

static void print_marked(FILE *o, const Op *op, int del) {
    int in = 0;
    for (int k = 0; k < op->ntk; k++) {
        int f = op->tf[k];
        if (f && !in) { fputs(del ? "[-" : "{+", o); in = 1; }
        if (!f && in) { fputs(del ? "-]" : "+}", o); in = 0; }
        fwrite(op->tk[k].s, 1, (size_t)op->tk[k].len, o);
    }
    if (in) fputs(del ? "-]" : "+}", o);
}

void print_diff(FILE *o, const Pair *p, const OpList *ops, const MoveList *mv,
                const char *old_name, const char *new_name, int ctx, int color) {
    int n = ops->n;
    fprintf(o, "--- %s\n+++ %s\n", old_name, new_name);
    if (mv && mv->n > 0) {
        fprintf(o, "# %d moved block%s ('<' = moved out of here, '>' = moved in here):\n",
                mv->n, mv->n == 1 ? "" : "s");
        for (int t = 0; t < mv->n; t++)
            fprintf(o, "#   old %d-%d  ->  new %d-%d  (%d lines)\n",
                    mv->v[t].a + 1, mv->v[t].a + mv->v[t].len,
                    mv->v[t].b + 1, mv->v[t].b + mv->v[t].len, mv->v[t].len);
    }

    unsigned char *show = calloc((size_t)n + 1, 1);
    int last = -1000000000;
    for (int i = 0; i < n; i++) {
        if (ops->v[i].t != OP_EQ) last = i;
        if (i - last <= ctx) show[i] = 1;
    }
    int nxt = 1000000000;
    for (int i = n - 1; i >= 0; i--) {
        if (ops->v[i].t != OP_EQ) nxt = i;
        if (nxt - i <= ctx) show[i] = 1;
    }

    int i = 0, aL = 0, bL = 0;
    while (i < n) {
        if (!show[i]) {
            if (ops->v[i].t != OP_INS) aL++;
            if (ops->v[i].t != OP_DEL) bL++;
            i++;
            continue;
        }
        int e = i;
        while (e < n && show[e]) e++;
        int ac = 0, bc = 0;
        for (int t = i; t < e; t++) {
            if (ops->v[t].t != OP_INS) ac++;
            if (ops->v[t].t != OP_DEL) bc++;
        }
        fprintf(o, "@@ -%d,%d +%d,%d @@\n", ac ? aL + 1 : aL, ac, bc ? bL + 1 : bL, bc);
        for (int t = i; t < e; t++) {
            const Op *op = &ops->v[t];
            if (op->t == OP_EQ) {
                fputc(' ', o);
                print_text(o, p->a.ptr[op->a], p->a.len[op->a]);
            } else if (op->t == OP_DEL) {
                if (color) fputs(op->mv ? C_CYN : C_RED, o);
                fputc(op->mv ? '<' : '-', o);
                if (op->tk) print_marked(o, op, 1);
                else print_text(o, p->a.ptr[op->a], p->a.len[op->a]);
                if (color) fputs(C_RST, o);
            } else {
                if (color) fputs(op->mv ? C_MAG : C_GRN, o);
                fputc(op->mv ? '>' : '+', o);
                if (op->tk) print_marked(o, op, 0);
                else print_text(o, p->b.ptr[op->b], p->b.len[op->b]);
                if (color) fputs(C_RST, o);
            }
            fputc('\n', o);
            if (op->t != OP_INS) aL++;
            if (op->t != OP_DEL) bL++;
        }
        i = e;
    }
    free(show);
}

/* ------------------------------------------------------------------ */
/* verification                                                        */
/* ------------------------------------------------------------------ */
int verify_script(const Pair *p, const MatchList *ml, const OpList *ops,
                  char *err, size_t errsz) {
    int m = p->a.n, n = p->b.n;
    int pa = -1, pb = -1;
    for (int t = 0; t < ml->n; t++) {
        int a = ml->v[t].a, b = ml->v[t].b;
        if (a < 0 || a >= m || b < 0 || b >= n) { snprintf(err, errsz, "match %d out of range", t); return -1; }
        if (a <= pa || b <= pb) { snprintf(err, errsz, "matches are not strictly increasing at %d", t); return -1; }
        if (p->a.id[a] != p->b.id[b]) { snprintf(err, errsz, "match %d pairs different lines", t); return -1; }
        pa = a; pb = b;
    }
    /* replay: old file + script must give exactly the new file */
    int ia = 0, ib = 0;
    for (int t = 0; t < ops->n; t++) {
        const Op *o = &ops->v[t];
        if (o->t == OP_EQ) {
            if (o->a != ia || o->b != ib || p->a.id[o->a] != p->b.id[o->b]) { snprintf(err, errsz, "bad EQ at op %d", t); return -1; }
            ia++; ib++;
        } else if (o->t == OP_DEL) {
            if (o->a != ia) { snprintf(err, errsz, "bad DEL at op %d", t); return -1; }
            ia++;
        } else {
            if (o->b != ib) { snprintf(err, errsz, "bad INS at op %d", t); return -1; }
            ib++;
        }
    }
    if (ia != m || ib != n) { snprintf(err, errsz, "script does not consume both files"); return -1; }
    return 0;
}
