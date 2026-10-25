/*
 * adiff.h - public interface of Adaptive LCS Diff
 *
 * A memory-aware, move-aware diff engine built on LCS dynamic programming.
 */
#ifndef ADIFF_H
#define ADIFF_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Matches: pairs (line in old file, line in new file) that stay equal */
/* ------------------------------------------------------------------ */
typedef struct { int a, b; } Match;
typedef struct { Match *v; int n, cap; } MatchList;

void ml_init(MatchList *l);
void ml_push(MatchList *l, int a, int b);
void ml_free(MatchList *l);

/* ------------------------------------------------------------------ */
/* Input files. Equal lines share one integer id (exact interning).    */
/* ------------------------------------------------------------------ */
typedef struct {
    char *buf;      /* whole file contents                              */
    char **ptr;     /* start of each line (points into buf)             */
    int *len;       /* line length without the trailing \n or \r        */
    int *id;        /* interned id, equal lines have equal ids          */
    int n;          /* number of lines                                  */
} Lines;

typedef struct {
    Lines a, b;             /* old file, new file                       */
    int nids;               /* number of distinct lines in both files   */
    unsigned char *trivial; /* per id: line too short to anchor a move  */
} Pair;

int  pair_load(Pair *p, const char *old_path, const char *new_path);
void pair_free(Pair *p);

/* ------------------------------------------------------------------ */
/* LCS engines                                                         */
/* ------------------------------------------------------------------ */
typedef enum {
    ENG_AUTO,     /* adaptive selector (trim, then pick an engine)      */
    ENG_CLASSIC,  /* textbook full table, NO trimming (baseline)        */
    ENG_FULL,     /* full table after trimming                          */
    ENG_HIRSCH,   /* Hirschberg linear-space divide and conquer         */
    ENG_BAND,     /* banded DP with doubling band (Ukkonen style)       */
    ENG_MYERS     /* Myers O(ND) greedy algorithm (baseline, as in git) */
} Engine;

typedef struct {
    Engine used;      /* engine that really produced the answer         */
    int trim_prefix;  /* identical lines removed from the head          */
    int trim_suffix;  /* identical lines removed from the tail          */
    int core_m, core_n; /* sizes after trimming                         */
    int band_k;       /* final band width if the banded engine ran      */
} EngineInfo;

const char *engine_name(Engine e);
int engine_parse(const char *s, Engine *e);

/* Fills `out` with an ordered list of matches (an LCS) over the full
 * files. Returns 0 on success, -1 if an engine hit its memory limit. */
int compute_matches(const int *a, int m, const int *b, int n, int nids,
                    Engine eng, MatchList *out, EngineInfo *info);

/* ------------------------------------------------------------------ */
/* Move detection                                                      */
/* ------------------------------------------------------------------ */
typedef struct { int a, b, len; } Move;
typedef struct { Move *v; int n, cap; } MoveList;

void detect_moves(const int *a, int m, const int *b, int n,
                  const unsigned char *trivial,
                  const unsigned char *matched_a, const unsigned char *matched_b,
                  int min_len, MoveList *out);
void moves_free(MoveList *l);

/* ------------------------------------------------------------------ */
/* Token level diff                                                    */
/* ------------------------------------------------------------------ */
typedef struct { const char *s; int len; int kind; } Tok;

int tokenize(const char *s, int len, Tok **out);
/* Returns 1 when the two lines are similar enough (>= 50% of tokens
 * shared); then the token arrays and "changed" flags are filled. */
int token_pair(const char *sa, int la, const char *sb, int lb,
               Tok **ta, int *na, unsigned char **fa,
               Tok **tb, int *nb, unsigned char **fb);

/* ------------------------------------------------------------------ */
/* Edit script and output                                              */
/* ------------------------------------------------------------------ */
typedef enum { OP_EQ, OP_DEL, OP_INS } OpType;
typedef struct {
    OpType t;
    int a, b;            /* line indexes (a for EQ/DEL, b for EQ/INS)   */
    int mv;              /* move index + 1, or 0 when not part of a move */
    Tok *tk; int ntk;    /* token level view when paired with a line    */
    unsigned char *tf;
} Op;
typedef struct { Op *v; int n; } OpList;

int  build_ops(const Pair *p, const MatchList *ml, OpList *ops);
void ops_free(OpList *ops);
void annotate_moves(OpList *ops, const MoveList *mv, int m, int n);
void annotate_tokens(const Pair *p, OpList *ops);
int  ops_have_changes(const OpList *ops);
void print_diff(FILE *o, const Pair *p, const OpList *ops, const MoveList *mv,
                const char *old_name, const char *new_name, int ctx, int color);
/* Independent check: matches are valid and old file + script == new file. */
int  verify_script(const Pair *p, const MatchList *ml, const OpList *ops,
                   char *err, size_t errsz);

/* ------------------------------------------------------------------ */
/* Sub commands                                                        */
/* ------------------------------------------------------------------ */
int cmd_bench(int argc, char **argv);
int cmd_gen(int argc, char **argv);
int cmd_selftest(int argc, char **argv);

#endif
