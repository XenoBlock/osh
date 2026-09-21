/* osh - Oricade Shell. Lightweight bash-like shell in C. */
#ifndef OSH_H
#define OSH_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <limits.h>
#include <pwd.h>

#define OSH_VERSION "1.0.1"
#define OSH_NAME    "osh"
#define HIST_MAX    2000
#define HIST_FILE   ".osh_history"
#define RC_FILE     ".oshrc"

/* ---------------- util.c ---------------- */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
int   xstreq(const char *a, const char *b);

typedef struct { char *buf; size_t len, cap; } Str;
void  str_init(Str *s);
void  str_free(Str *s);
void  str_clear(Str *s);
void  str_grow(Str *s, size_t extra);
void  str_putc(Str *s, char c);
void  str_putn(Str *s, const char *t, size_t n);
void  str_puts(Str *s, const char *t);
void  str_printf(Str *s, const char *fmt, ...);
char *str_done(Str *s);           /* malloc'd, NUL-terminated; resets Str */

typedef struct { void **data; int len, cap; } Vec;
void  vec_init(Vec *v);
void  vec_push(Vec *v, void *p);
void  vec_free(Vec *v);
#define vec_at(v,i) ((v)->data[(i)])

typedef struct {
    char **keys; char **vals; unsigned char *used;
    size_t cap, size;
} Map;
void  map_init(Map *m);
void  map_free(Map *m);
void  map_put(Map *m, const char *k, const char *v);   /* copies both */
const char *map_get(Map *m, const char *k);
int   map_del(Map *m, const char *k);

size_t map_next_used(Map *m, size_t i);  /* next used slot index >= i, or m->cap */

char *tilde_expand(const char *s);          /* ~ or ~user (malloc'd) */
const char *home_dir(void);
int   is_directory(const char *p);
int   open_user_file(const char *path, int write);
int   is_executable(const char *p);

/* globbing without libc glob(): recursive matcher */
typedef struct {
    char **paths;
    size_t count, cap;
} Glob;
int  glob_pattern(const char *pattern, Glob *g);  /* 0 = no match found */
void glob_free(Glob *g);
int  has_glob_chars(const char *s);
/* expand one (possibly globbing) field into out */
void glob_one(const char *field, const char *qmask, int do_glob, Vec *out);

/* ---------------- var.c ---------------- */
typedef struct { char *text; int q; } Seg;  /* q: 0 unq, 1 double, 2 single */
typedef struct { Seg *s; int n, cap; } Word;

void word_free(Word *w);
void word_add(Word *w, const char *t, int q);
char *word_raw(Word *w);                    /* text with quotes removed */
int  word_unquoted(Word *w);                /* no quoted segments at all */

/* expansion flags */
#define EX_SPLIT 1
#define EX_GLOB  2
void expand_word(Word *w, Vec *out, int flags);
void expand_str(const char *s, Vec *out, int flags);
long long arith_eval(const char *expr);
void var_set(const char *name, const char *val);
void var_setl(const char *name, size_t nlen, const char *val);
void var_import_env(void);
const char *var_get(const char *name);
void var_mark_export(const char *name);
void var_export_all(void);
void var_unset(const char *name);

extern Map g_vars, g_aliases, g_funs, g_exported, g_homes;
extern char **g_posargs;     /* $1.. */
extern int    g_nposargs;
extern char  *g_arg0;        /* $0 */
extern char  *g_ifs;

/* variable scopes for functions */
typedef struct Scope {
    Map vars;
    struct Scope *prev;
} Scope;
extern Scope *g_scopes;
void scope_push(void);
void scope_pop(void);
void scope_set_local(const char *name, const char *val);
int  scope_is_local(const char *name);
void var_names_matching(const char *prefix, Vec *out);

/* special parameter helpers */
char *special_param(char c);   /* $? $# $! $$ $- $@ $* */
char *positional_param(int n); /* malloc'd or NULL */
void set_shell_options_from(const char *s);
void clear_shell_options_from(const char *s);

/* tilde/parameter expansion entry used by the lexer */
char *do_word_expansion(const char *raw, int allow_split_and_glob, Vec *out);

/* command substitution $(...) and `...` */
char *capture_subshell(const char *body, int capture_output);

/* run a string of shell code in the current process (used by $( ) and eval) */
void run_string(const char *code);

/* file existence tests for glob-less ${var:-f} style ops are in var.c */
int test_file_op(int op, const char *path);
const char *option_string(void);

/* ---------------- lex.c ---------------- */
typedef enum {
    T_WORD, T_PIPE, T_AND, T_OR, T_SEMI, T_AMP, T_LP, T_RP, T_LB, T_RB,
    T_REDIR, T_NEWLINE, T_EOF
} TokType;

typedef struct {
    TokType type;
    Word    word;            /* for T_WORD / redir target */
    char    op[8];           /* operator text for T_REDIR */
    int     fd;              /* fd for T_REDIR, -1 if default */
    int     line;
    char   *heredoc;         /* body captured by the lexer for << / <<- */
} Token;

typedef struct Reader Reader;
struct Reader {
    char  *buf;
    size_t pos, len;
    int    eof;
    int    no_more;          /* -c mode: never ask for more */
    char *(*fill)(Reader *, const char *prompt);
    void *data;
};

void   reader_init_buf(Reader *r, const char *s);
void   reader_init_file(Reader *r, FILE *f);
void   reader_init_tty(Reader *r);
int    reader_getline(Reader *r, const char *prompt, Str *out);

typedef struct Lexer {
    Reader *r;
    Token   tok;             /* current token */
    Token   peek;
    int     have_peek;
    int     error;
    int     eof;
    int     in_heredoc;     /* reading a body: suppress lookahead filling */
    /* character buffer: one physical line is appended at a time */
    char   *buf;
    size_t  pos, len, cap;
    int     line_no;
} Lexer;

void   lex_init(Lexer *lx, Reader *r);
int    lex_fill(Lexer *lx);
void   lex_next(Lexer *lx);  /* fill lx->tok */
void   lex_peek(Lexer *lx);
void   token_free(Token *t);
char  *lex_read_heredoc(Lexer *lx, const char *term, int strip_tabs);
void   advance_token(Lexer *lx);
int    lex_getline(Lexer *lx, Str *out);
int    lx_looks_like_fn(Lexer *lx);

/* ---------------- parse.c ---------------- */
typedef struct Redir {
    int   fd;
    int   type;              /* see R_* below */
    Word  target;
    char *heredoc;           /* body (NULL unless heredoc) */
} Redir;

enum { R_IN, R_OUT, R_APP, R_ERR, R_ERRAPP, R_HEREDOC, R_DHEREDOC,
       R_HERESTR, R_DUPIN, R_DUPOUT, R_CLOBBER, R_INOUT };

typedef struct Node {
    int     kind;            /* N_* below */
    struct Node *a, *b, *c;
    Word   *args;            /* N_CMD / N_FOR / N_CASE arguments (unexpanded) */
    int     nargs;
    int     nargs_set;       /* N_FOR: explicit `in` list present */
    char  **assigns;         /* N_CMD prefix assignments, "name=word" (raw) */
    int     nassigns;
    Redir  *redirs;
    int     nredirs;
    char   *var;             /* N_FOR variable name / N_ARITH_FOR init */
    char   *cond;            /* N_ARITH_FOR condition */
    char   *step;            /* N_ARITH_FOR increment */
} Node;

enum { N_NONE, N_CMD, N_PIPE, N_AND, N_OR, N_SEQ, N_BG, N_SUB, N_GROUP,
       N_IF, N_WHILE, N_UNTIL, N_FOR, N_CASE, N_TEST, N_ARITH_FOR };

Node *parse_line(Lexer *lx);       /* may request more input via reader */
Node *parse_case_list(Lexer *lx);
void  node_free(Node *n);

extern int g_parse_heredocs_pending;

/* ---------------- job control ---------------- */
typedef struct {
    pid_t pid;
    int   job;
    char  *cmd;
    int   running;      /* 1 running, 0 stopped, -1 done */
    int   reported;
} Job;
#define MAX_JOBS 256
extern Job jobs[MAX_JOBS];
extern int  next_job;
int  job_add(pid_t pid, Node *n);

/* ---------------- exec.c ---------------- */
extern int   g_status;
extern pid_t g_last_bg;
extern int   g_opt_errexit, g_opt_xtrace, g_opt_unset;
extern int   g_opt_noglob, g_opt_allexport, g_opt_ignoreeof, g_opt_notify;
extern int   g_opt_braceexpand, g_opt_noclobber, g_opt_pipefail;
extern int   g_opt_histexpand, g_opt_verbose;
extern int   g_opt_autoopen;
extern int   g_lineno;
extern int   g_interactive;
extern jmp_buf g_exit_jmp;
extern int   g_exit_jmp_set;

int   exec_node(Node *n, int bg);
int   exec_simple(Node *n);        /* for pipelines: returns via fork */
void  exec_subshell_body(Node *n); /* run node list in current process */
int   is_builtin(const char *name);
int   run_builtin(int argc, char **argv);
char *command_resolve(const char *name);   /* malloc'd path or NULL */
void  jobs_reap(void);
void  jobs_print_changes(void);
void  sigchld_arm(void);
void  sigchld_block(void);
void  sigchld_unblock(void);
void  shell_signals_init(void);
void  trap_run(int sig);

/* flow control signalling */
enum { FLOW_NONE = 0, FLOW_BREAK, FLOW_CONTINUE, FLOW_RETURN };
extern int g_flow, g_flow_level;
#define CHECK_FLOW() if (g_flow != FLOW_NONE) return g_status

/* ---------------- builtin.c ---------------- */
typedef struct { const char *name; int (*fn)(int, char **); } Builtin;
extern Builtin builtins[];

/* ---------------- edit.c ---------------- */
void  edit_init(void);
char *edit_getline(const char *prompt);     /* malloc'd, may be NULL on EOF */
void  edit_add_history(const char *line);
void  edit_save_history(void);
void  edit_load_history(void);
char *prompt_string(const char *ps);        /* expand PS escapes */

/* ---------------- session.c ---------------- */
int session_client(const char *id);         /* attach to (or start) a session */
int session_id_ok(const char *id);          /* conservative id -> file name check */
int session_list(void);                     /* print live session IDs */
void  edit_disable(void);
int   edit_complete(const char *buf, int pos, char **out, int *common);

/* ---------- exec.c extras ---------- */
int   is_builtin_safe(Node *n);
int   job_add(pid_t pid, Node *n);
void  node_to_str(Node *n, Str *s);
void  trap_set(const char *name, const char *body);
const char *trap_get(const char *name);
extern Map g_traps;
void  var_mark_exportl(const char *name, size_t nlen);
int   gmatch_c(const char *str, const char *pat);
int   match_prefix(const char *str, const char *pat);
char *expand_param_str(const char *body);
void  node_to_source(Node *n, Str *s);

/* ---------------- builtin support ---------------- */
void  history_print(void);
void  jobs_print_all(void);
pid_t job_pid(int job);
void  fg_job(int job);
void  bg_job(int job);
void  wait_for_jobs(void);
int   read_input_line(Str *out);
int   test_eval(int argc, char **argv);

/* ---------------- osh.c ---------------- */
extern FILE *g_out;     /* stdout redirect target for tests */
const char *shell_name(void);
void osh_die(const char *fmt, ...);

#endif /* OSH_H */
