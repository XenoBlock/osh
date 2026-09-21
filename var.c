/* var.c - variable storage, parameter expansion, command substitution,
 *         arithmetic, word splitting and globbing.
 *
 * Expansion model: a word is expanded into (text, quoted-mask) pairs.
 * The mask records which output characters came from quotes or from
 * variable/command substitution, so that word splitting and globbing
 * only apply to unquoted, expansion-produced text. */
#include "osh.h"
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <pwd.h>

Map   g_vars    = {0};
Map   g_aliases = {0};
Map   g_funs    = {0};
Map   g_exported = {0};
char **g_posargs = NULL;
int    g_nposargs = 0;
char  *g_arg0 = NULL;
char  *g_ifs  = NULL;

Scope *g_scopes = NULL;

int g_opt_errexit = 0, g_opt_xtrace = 0, g_opt_unset = 0, g_opt_noglob = 0;
int g_opt_allexport = 0, g_opt_ignoreeof = 0, g_opt_notify = 0;
int g_opt_braceexpand = 1, g_opt_clobber = 1, g_opt_pipefail = 0;
int g_opt_histexpand = 0, g_opt_verbose = 0;
int g_opt_autoopen = 1;
int g_lineno = 0;

/* ---------- scopes ---------- */
void scope_push(void) {
    Scope *s = xmalloc(sizeof(Scope));
    map_init(&s->vars);
    s->prev = g_scopes;
    g_scopes = s;
}
void scope_pop(void) {
    Scope *s = g_scopes;
    if (!s) return;
    g_scopes = s->prev;
    map_free(&s->vars);
    free(s);
}
void scope_set_local(const char *name, const char *val) {
    if (!g_scopes) { var_set(name, val); return; }
    map_put(&g_scopes->vars, name, val);
}
int scope_is_local(const char *name) {
    for (Scope *s = g_scopes; s; s = s->prev)
        if (map_get(&s->vars, name)) return 1;
    return 0;
}

/* ---------- variables ---------- */
/* lazy import of the process environment; called on first var access */
extern char **environ;
static int g_env_imported = 0;
void var_import_env(void) {
    if (g_env_imported) return;
    g_env_imported = 1;
    for (char **e = environ; e && *e; e++) {
        char *eq = strchr(*e, '=');
        if (!eq) continue;
        size_t nl = eq - *e;
        char *nm = xstrndup(*e, nl);
        map_put(&g_vars, nm, eq + 1);
        free(nm);
    }
}

void var_set(const char *name, const char *val) {
    for (Scope *s = g_scopes; s; s = s->prev)
        if (map_get(&s->vars, name)) { map_put(&s->vars, name, val); return; }
    map_put(&g_vars, name, val);
    if (g_opt_allexport) { map_put(&g_exported, name, "1"); setenv(name, val ? val : "", 1); }
}
void var_setl(const char *name, size_t nlen, const char *val) {
    char *tmp = xstrndup(name, nlen);
    var_set(tmp, val);
    free(tmp);
}
const char *var_get(const char *name) {
    if (!g_vars.cap && !g_vars.keys) var_import_env();
    for (Scope *s = g_scopes; s; s = s->prev) {
        const char *v = map_get(&s->vars, name);
        if (v) return v;
    }
    return map_get(&g_vars, name);
}
void var_mark_exportl(const char *name, size_t nlen) {
    char *tmp = xstrndup(name, nlen);
    var_mark_export(tmp);
    free(tmp);
}

void var_mark_export(const char *name) {
    map_put(&g_exported, name, "1");
    const char *v = var_get(name);
    setenv(name, v ? v : "", 1);
}
void var_export_all(void) {
    for (size_t i = map_next_used(&g_exported, 0); i < g_exported.cap;
         i = map_next_used(&g_exported, i + 1)) {
        const char *val = var_get(g_exported.keys[i]);
        setenv(g_exported.keys[i], val ? val : "", 1);
    }
}
void var_unset(const char *name) {
    for (Scope *s = g_scopes; s; s = s->prev) map_del(&s->vars, name);
    map_del(&g_vars, name);
    map_del(&g_exported, name);
    unsetenv(name);
}

/* ---------- options ---------- */
static struct { char letter; int *flag; } optlist[] = {
    {'e', &g_opt_errexit},   {'u', &g_opt_unset},
    {'x', &g_opt_xtrace},    {'f', &g_opt_noglob},
    {'a', &g_opt_allexport}, {'B', &g_opt_braceexpand},
    {'C', &g_opt_clobber},   {'p', &g_opt_pipefail},
    {'b', &g_opt_notify},    {'h', &g_opt_ignoreeof},
    {'H', &g_opt_histexpand},{'v', &g_opt_verbose},
    {'O', &g_opt_autoopen},
    {0, NULL}
};
const char *option_string(void) {
    static char buf[32];
    int j = 0;
    for (int i = 0; optlist[i].letter; i++)
        if (*optlist[i].flag) buf[j++] = optlist[i].letter;
    buf[j] = 0;
    return buf;
}
void set_shell_options_from(const char *s) {
    for (; *s; s++)
        for (int i = 0; optlist[i].letter; i++)
            if (optlist[i].letter == *s) { *optlist[i].flag = 1; break; }
}

static int unset_strict(void) { return g_opt_unset; }

/* ---------- special parameters ---------- */
extern int   g_status;
extern pid_t g_last_bg;
extern int   g_interactive;

char *special_param(char c) {
    Str s; str_init(&s);
    switch (c) {
    case '?': str_printf(&s, "%d", g_status); break;
    case '#': str_printf(&s, "%d", g_nposargs); break;
    case '@':
    case '*': {
        int sep = (c == '@') ? 1 : 0;   /* * joins with first char of IFS */
        const char *ifs = (g_ifs && *g_ifs) ? g_ifs : " \t\n";
        for (int i = 0; i < g_nposargs; i++) {
            if (i) str_putc(&s, sep ? 1 : ifs[0]);  /* 0x01 = field sep marker */
            str_puts(&s, g_posargs[i]);
        }
        break;
    }
    case '$': str_printf(&s, "%d", (int)getpid()); break;
    case '!': if (g_last_bg) str_printf(&s, "%d", (int)g_last_bg); break;
    case '0': if (g_arg0) str_puts(&s, g_arg0); break;
    case '-': str_puts(&s, option_string()); break;
    default:  return NULL;
    }
    return str_done(&s);
}

char *positional_param(int n) {
    if (n == 0) return xstrdup(g_arg0 ? g_arg0 : "");
    if (n < 1 || n > g_nposargs) return NULL;
    return xstrdup(g_posargs[n - 1]);
}

/* ---------- IFS splitting ---------- */
static int is_ifs(char c) {
    const char *ifs = (g_ifs && *g_ifs) ? g_ifs : " \t\n";
    for (; *ifs; ifs++) if (*ifs == c) return 1;
    return 0;
}

/* ---------- globbing ---------- */
int has_glob_chars(const char *s) {
    for (; *s; s++) {
        if (*s == '*' || *s == '?' || *s == '[') return 1;
        if (*s == '\\') s++;
    }
    return 0;
}

static void glob_add(Glob *g, const char *path) {
    if (g->count + 1 >= g->cap) {
        g->cap = g->cap ? g->cap * 2 : 16;
        g->paths = xrealloc(g->paths, g->cap * sizeof(char *));
    }
    g->paths[g->count++] = xstrdup(path);
}

/* match a character class beginning at pat (after '[').
 * on success advances *pp past ']' and returns 1; matched flag set. */
static int class_match(const char **pp, char c, int *matched) {
    const char *p = *pp;
    int negate = 0;
    if (*p == '!' || *p == '^') { negate = 1; p++; }
    int found = 0, ok = 0;
    while (*p && *p != ']') {
        if (p[1] == '-' && p[2] && p[2] != ']') {
            if ((unsigned char)c >= (unsigned char)p[0] &&
                (unsigned char)c <= (unsigned char)p[2]) found = 1;
            p += 3;
        } else {
            if (*p == c) found = 1;
            p++;
        }
        ok = 1;
    }
    if (!ok) return 0;                     /* [] is not a valid class */
    if (*p == ']') p++;
    *pp = p;
    *matched = negate ? !found : found;
    return 1;
}

static int gmatch(const char *pat, const char *str) {
    while (*pat) {
        switch (*pat) {
        case '*':
            while (*pat == '*') pat++;
            if (!*pat) return 1;
            for (const char *s = str; *s; s++)
                if (gmatch(pat, s)) return 1;
            return gmatch(pat, str);
        case '?':
            if (!*str) return 0;
            pat++; str++;
            break;
        case '[': {
            int m;
            const char *save = pat + 1;
            if (!class_match(&save, *str, &m)) return 0;
            if (!m) return 0;
            pat = save;
            str++;
            break;
        }
        case '\\':
            pat++;
            if (!*pat) return 0;
            /* fall through */
        default:
            if (*pat != *str) return 0;
            pat++; str++;
            break;
        }
    }
    return *str == 0;
}

/* Expand pattern against the filesystem. Results are added to g in sorted
 * order (directory read order is used; osh sorts afterwards). */
static int glob_rec(const char *pat, const char *dir, Glob *g) {
    const char *slash = strchr(pat, '/');
    size_t seglen = slash ? (size_t)(slash - pat) : strlen(pat);
    char *seg = xstrndup(pat, seglen);
    const char *rest = slash ? slash + 1 : NULL;
    DIR *dp = opendir(*dir ? dir : ".");
    if (!dp) { free(seg); return 0; }
    int matched = 0;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (de->d_name[0] == '.' && seg[0] != '.') continue;
        if (!gmatch(seg, de->d_name)) continue;
        Str full; str_init(&full);
        if (*dir) { str_puts(&full, dir); if (dir[strlen(dir)-1] != '/') str_putc(&full, '/'); }
        str_puts(&full, de->d_name);
        if (!rest) {
            glob_add(g, full.buf);
            matched++;
        } else if (is_directory(full.buf)) {
            if (glob_rec(rest, full.buf, g)) matched++;
        }
        str_free(&full);
    }
    closedir(dp);
    free(seg);
    return matched;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int glob_pattern(const char *pattern, Glob *g) {
    if (!has_glob_chars(pattern)) return 0;
    /* literal leading directory part */
    Str base; str_init(&base);
    const char *p = pattern;
    for (;;) {
        const char *slash = strchr(p, '/');
        if (!slash) break;
        size_t seg = slash - p;
        char *tmp = xstrndup(p, seg);
        int wild = has_glob_chars(tmp);
        free(tmp);
        if (wild) break;
        str_putn(&base, p, seg + 1);
        p = slash + 1;
    }
    int rc = glob_rec(p, base.buf ? base.buf : ".", g);
    if (g->count > 1)
        qsort(g->paths, g->count, sizeof(char *), cmp_str);
    str_free(&base);
    return rc;
}

void glob_free(Glob *g) {
    for (size_t i = 0; i < g->count; i++) free(g->paths[i]);
    free(g->paths);
    g->paths = NULL; g->count = 0; g->cap = 0;
}

/* ---------- arithmetic ---------- */
typedef struct { const char *s; int err; } AP;
static long long ap_expr(AP *a);

static void ap_skip(AP *a) {
    while (*a->s == ' ' || *a->s == '\t') a->s++;
}

static long long ap_var_get(const char *st, const char *en) {
    char *nm = xstrndup(st, en - st);
    const char *v = NULL;
    char *owned = NULL;
    if (isdigit((unsigned char)*nm)) {
        owned = positional_param(atoi(nm));
        v = owned;
    } else {
        v = var_get(nm);
    }
    long long val;
    if (!v || !*v) val = 0;
    else {
        AP a = { v };
        val = ap_expr(&a);
    }
    free(nm);
    free(owned);
    return val;
}

static long long ap_primary(AP *a) {
    ap_skip(a);
    if (*a->s == '(') {
        a->s++;
        long long v = ap_expr(a);
        ap_skip(a);
        if (*a->s == ')') a->s++;
        return v;
    }
    if (*a->s == '!') { a->s++; return !ap_primary(a); }
    if (*a->s == '-') { a->s++; return -ap_primary(a); }
    if (*a->s == '+') { a->s++; return ap_primary(a); }
    if (*a->s == '~') { a->s++; return ~ap_primary(a); }
    if (isdigit((unsigned char)*a->s)) {
        char *end;
        long long v = strtoll(a->s, &end, 0);
        a->s = end;
        return v;
    }
    if (*a->s == '$') {
        /* $var or $1 style parameter inside arithmetic */
        a->s++;
        const char *st = a->s;
        if (isdigit((unsigned char)*a->s)) {
            while (isdigit((unsigned char)*a->s)) a->s++;
        } else {
            while (isalnum((unsigned char)*a->s) || *a->s == '_') a->s++;
        }
        return ap_var_get(st, a->s);
    }
    if (isalpha((unsigned char)*a->s) || *a->s == '_') {
        const char *st = a->s;
        while (isalnum((unsigned char)*a->s) || *a->s == '_') a->s++;
        const char *end = a->s;
        ap_skip(a);
        /* postfix ++ / -- : i++ increments and yields the old value */
        if ((a->s[0] == '+' && a->s[1] == '+') ||
            (a->s[0] == '-' && a->s[1] == '-')) {
            long long old = ap_var_get(st, end);
            long long nv = (a->s[0] == '+') ? old + 1 : old - 1;
            char nmbuf[64];
            snprintf(nmbuf, sizeof nmbuf, "%lld", nv);
            var_setl(st, end - st, nmbuf);
            a->s += 2;
            return old;
        }
        return ap_var_get(st, end);
    }
    return 0;
}

static long long ap_mul(AP *a) {
    long long v = ap_primary(a);
    for (;;) {
        ap_skip(a);
        char op = *a->s;
        if (op != '*' && op != '/' && op != '%') return v;
        a->s++;
        long long r = ap_primary(a);
        if (op == '*') v *= r;
        else if (r == 0) { a->err = 1; v = 0; }
        else if (op == '/') v /= r;
        else v %= r;
    }
}

static long long ap_add(AP *a) {
    long long v = ap_mul(a);
    for (;;) {
        ap_skip(a);
        char c = *a->s;
        if ((c == '+' || c == '-') && a->s[1] != c) {
            a->s++;
            long long r = ap_mul(a);
            if (c == '+') v += r; else v -= r;
        } else return v;
    }
}

static long long ap_shift(AP *a) {
    long long v = ap_add(a);
    ap_skip(a);
    if (a->s[0] == '<' && a->s[1] == '<') { a->s += 2; return v << ap_add(a); }
    if (a->s[0] == '>' && a->s[1] == '>') { a->s += 2; return v >> ap_add(a); }
    return v;
}

static long long ap_cmp(AP *a) {
    long long v = ap_shift(a);
    for (;;) {
        ap_skip(a);
        if (a->s[0] == '<' && a->s[1] == '=') { a->s += 2; v = (v <= ap_shift(a)); }
        else if (a->s[0] == '>' && a->s[1] == '=') { a->s += 2; v = (v >= ap_shift(a)); }
        else if (a->s[0] == '<' && a->s[1] != '<') { a->s++; v = (v < ap_shift(a)); }
        else if (a->s[0] == '>' && a->s[1] != '>') { a->s++; v = (v > ap_shift(a)); }
        else return v;
    }
}

static long long ap_eq(AP *a) {
    long long v = ap_cmp(a);
    for (;;) {
        ap_skip(a);
        if (a->s[0] == '=' && a->s[1] == '=') { a->s += 2; v = (v == ap_cmp(a)); }
        else if (a->s[0] == '!' && a->s[1] == '=') { a->s += 2; v = (v != ap_cmp(a)); }
        else return v;
    }
}

static long long ap_band(AP *a) {
    long long v = ap_eq(a);
    ap_skip(a);
    if (*a->s == '&' && a->s[1] != '&') { a->s++; return v & ap_band(a); }
    return v;
}

static long long ap_bxor(AP *a) {
    long long v = ap_band(a);
    ap_skip(a);
    if (*a->s == '^') { a->s++; return v ^ ap_bxor(a); }
    return v;
}

static long long ap_bor(AP *a) {
    long long v = ap_bxor(a);
    ap_skip(a);
    if (*a->s == '|' && a->s[1] != '|') { a->s++; return v | ap_bor(a); }
    return v;
}

static long long ap_land(AP *a) {
    long long v = ap_bor(a);
    ap_skip(a);
    if (a->s[0] == '&' && a->s[1] == '&') { a->s += 2; return v && ap_land(a); }
    return v;
}

static long long ap_lor(AP *a) {
    long long v = ap_land(a);
    ap_skip(a);
    if (a->s[0] == '|' && a->s[1] == '|') { a->s += 2; return v || ap_lor(a); }
    return v;
}

static long long ap_cond(AP *a) {
    long long v = ap_lor(a);
    ap_skip(a);
    if (*a->s == '?') {
        a->s++;
        long long t = ap_cond(a);
        ap_skip(a);
        if (*a->s == ':') a->s++;
        return v ? t : ap_cond(a);
    }
    return v;
}

static long long ap_assign(AP *a) {
    /* recognize NAME = expr / NAME OP= expr and assign the variable */
    ap_skip(a);
    const char *st = a->s;
    if (isalpha((unsigned char)*a->s) || *a->s == '_') {
        while (isalnum((unsigned char)*a->s) || *a->s == '_') a->s++;
        const char *en = a->s;
        ap_skip(a);
        char c = *a->s;
        int compound = c && a->s[1] == '=' &&
            (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' ||
             c == '&' || c == '|' || c == '^');
        if (c == '=' || compound) {
            size_t nl = en - st;
            a->s += compound ? 2 : 1;
            long long r = ap_assign(a);
            long long v = r;
            if (compound) {
                char nm[128];
                snprintf(nm, sizeof nm, "%.*s", (int)nl, st);
                long long cur = 0;
                const char *cv = var_get(nm);
                if (cv) cur = atoll(cv);
                switch (c) {
                case '+': v = cur + r; break;
                case '-': v = cur - r; break;
                case '*': v = cur * r; break;
                case '/': v = r ? cur / r : 0; break;
                case '%': v = r ? cur % r : 0; break;
                case '&': v = cur & r; break;
                case '|': v = cur | r; break;
                case '^': v = cur ^ r; break;
                }
            }
            char nv[32];
            snprintf(nv, sizeof nv, "%lld", v);
            var_setl(st, nl, nv);
            return v;
        }
        a->s = st;   /* not an assignment: reparse as expression */
    }
    return ap_cond(a);
}

static long long ap_expr(AP *a) { return ap_assign(a); }

long long arith_eval(const char *expr) {
    AP a = { expr, 0 };
    long long v = ap_expr(&a);
    if (a.err) fprintf(stderr, "osh: arithmetic: division by zero\n");
    return v;
}

/* ---------- parameter expansion ${...} ---------- */
/* expand the inside of ${...}; returns a malloc'd string */
static char *expand_param_body(const char *body) {
    if (!*body) return xstrdup("");

    Str out; str_init(&out);
    const char *p = body;
    int indirect = 0;
    if (*p == '!') { indirect = 1; p++; }

    if (*p == '#') {     /* length */
        p++;
        if (*p == '@' || *p == '*') { str_printf(&out, "%d", g_nposargs); return str_done(&out); }
        const char *ns = p;
        while (isalnum((unsigned char)*p) || *p == '_') p++;
        char *nm = xstrndup(ns, p - ns);
        const char *v = var_get(nm);
        str_printf(&out, "%zu", v ? strlen(v) : 0);
        free(nm);
        return str_done(&out);
    }

    /* parameter name */
    const char *st = p;
    if (isdigit((unsigned char)*p)) while (isdigit((unsigned char)*p)) p++;
    else if (*p == '@' || *p == '*' || *p == '#' || *p == '?' ||
             *p == '!' || *p == '-' || *p == '$') p++;
    else while (isalnum((unsigned char)*p) || *p == '_') p++;
    char *name = xstrndup(st, p - st);

    if (indirect) {
        const char *v = var_get(name);
        free(name);
        name = xstrdup(v ? v : "");
    }

    char *special = NULL;
    const char *val = NULL;
    if (isdigit((unsigned char)name[0])) {
        special = positional_param(atoi(name));
        val = special;
    } else if (strchr("@*#?!-$", name[0])) {
        special = special_param(name[0]);
        val = special;
    } else {
        val = var_get(name);
        if (!val && unset_strict() && *name)
            fprintf(stderr, "osh: %s: unbound variable\n", name);
    }

    if (!*p) {       /* simple ${name} */
        char *r = xstrdup(val ? val : "");
        free(special); free(name);
        return r;
    }

    /* ${var:offset:length} substring extraction */
    if (*p == ':') {
        const char *peek = p + 1;
        while (*peek == ' ' || *peek == '\t') peek++;
        if (isdigit((unsigned char)*peek) || (*peek == '-' && isdigit((unsigned char)peek[1])) ||
            *peek == '\0') {
            p = peek;
            long off = 0;
            int neg = (*p == '-');
            if (neg) p++;
            while (isdigit((unsigned char)*p)) { off = off * 10 + (*p - '0'); p++; }
            if (neg) off = -off;
            long len = -1;
            if (*p == ':') {
                p++;
                len = 0;
                while (isdigit((unsigned char)*p)) { len = len * 10 + (*p - '0'); p++; }
            }
            const char *base = val ? val : "";
            size_t bl = strlen(base);
            size_t start = off < 0 ? (size_t)((long)bl + off < 0 ? 0 : (long)bl + off)
                                   : (size_t)off;
            if (start > bl) start = bl;
            size_t take = (len < 0) ? bl - start : (size_t)len;
            if (start + take > bl) take = bl - start;
            char *r = xstrndup(base + start, take);
            free(special); free(name);
            return r;
        }
    }

    /* ${var/pat/repl} and ${var//pat/repl} substitution */
    if (*p == '/') {
        int all = (p[1] == '/');
        p += 1 + all;
        const char *patstart = p;
        while (*p && *p != '/') p++;
        char *pat = xstrndup(patstart, p - patstart);
        if (*p == '/') p++;
        const char *rep = p;
        const char *base = val ? val : "";
        Str res; str_init(&res);
        size_t i = 0, bl = strlen(base);
        if (!*pat) {           /* empty pattern: no substitution */
            str_puts(&res, base);
        } else for (;;) {
            int n = match_prefix(base + i, pat);
            if (n >= 0) {
                str_puts(&res, rep);
                i += (size_t)n;
                if (!n) i++;             /* always advance to avoid spinning */
                if (!all) { str_puts(&res, base + i); break; }
                if (i >= bl) break;
                continue;
            }
            if (i >= bl) break;
            str_putc(&res, base[i]);
            i++;
        }
        free(pat);
        char *r = str_done(&res);
        free(special); free(name);
        return r;
    }

    int colon = 0;
    if (*p == ':') { colon = 1; p++; }
    char op = *p;
    if (op) p++;

    /* argument: expand recursively (no splitting) */
    char *arg = NULL;
    if (*p) {
        Vec sub; vec_init(&sub);
        expand_str(p, &sub, 0);
        if (sub.len) {
            Str j; str_init(&j);
            for (int i = 0; i < sub.len; i++) {
                if (i) str_putc(&j, ' ');
                str_puts(&j, (char *)sub.data[i]);
            }
            arg = str_done(&j);
        } else arg = xstrdup("");
        vec_free(&sub);
    }

    int isnull = !val || !*val;
    if (colon && val && !*val) isnull = 1;

    char *result;
    switch (op) {
    case '-': result = xstrdup(isnull ? (arg ? arg : "") : val); break;
    case '=':
        if (isnull) {
            var_set(name, arg ? arg : "");
            result = xstrdup(arg ? arg : "");
        } else result = xstrdup(val);
        break;
    case '+': result = xstrdup(!isnull ? (arg ? arg : "") : ""); break;
    case '?':
        if (isnull) {
            fprintf(stderr, "osh: %s: %s\n", name, arg && *arg ? arg : "parameter null or not set");
        }
        result = xstrdup(isnull ? "" : val);
        break;
    default:  result = xstrdup(val ? val : ""); break;
    }
    free(special); free(name); free(arg);
    return result;
}

/* public wrapper for self-test */
char *expand_param_str(const char *body) { return expand_param_body(body); }

/* ---------- command substitution ---------- */
char *capture_subshell(const char *body, int capture_output) {
    int fds[2];
    if (pipe(fds) != 0) return NULL;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return NULL; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        g_interactive = 0;
        run_string(body);
        fflush(NULL);
        _exit(g_status);
    }
    close(fds[1]);
    Str out; str_init(&out);
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0)
        str_putn(&out, buf, (size_t)n);
    close(fds[0]);
    int st;
    waitpid(pid, &st, 0);
    g_status = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
    while (out.len && (out.buf[out.len-1] == '\n' || out.buf[out.len-1] == '\r'))
        out.len--;
    if (!out.len) { str_free(&out); return xstrdup(""); }
    out.buf[out.len] = 0;
    return str_done(&out);
}


/* ---------- the core expander ---------- */
/* qmode: 0 unquoted, 1 double-quoted, 2 single-quoted.
 * cur receives expanded text; qmask receives a per-character marker:
 *   1 = quoted (literal, no split/glob)
 *   2 = produced by expansion (split/glob allowed)
 *   0 = literal unquoted */
void expand_into(const char *raw, int qmode, Str *cur, Str *qmask) {
    const char *p = raw;
    while (*p) {
        if (qmode == 2) {
            str_putc(cur, *p);
            str_putc(qmask, 1);
            p++;
            continue;
        }
        if (*p == '\\' && qmode == 0) {
            p++;
            if (*p) { str_putc(cur, *p); str_putc(qmask, 1); p++; }
            continue;
        }
        if (*p == '\\' && qmode == 1) {
            p++;
            if (*p == '$' || *p == '`' || *p == '"' || *p == '\\') {
                str_putc(cur, *p); str_putc(qmask, 1); p++;
            } else if (*p == '\n') {
                p++;
            } else {
                str_putc(cur, '\\'); str_putc(qmask, 1);
            }
            continue;
        }
        if (*p == '$' && p[1] == '(' && p[2] == '(') {
            /* $(( )) arithmetic */
            p += 3;
            int depth = 1;
            const char *st = p;
            while (*p) {
                if (*p == '(') depth++;
                else if (*p == ')') { depth--; if (!depth) break; }
                p++;
            }
            char *body = xstrndup(st, p - st);
            if (*p == ')') p++;                 /* consume closing '))' */
            if (*p == ')') p++;
            char *r = xmalloc(32);
            snprintf(r, 32, "%lld", arith_eval(body));
            free(body);
            size_t rl = strlen(r);
            str_putn(cur, r, rl);
            for (size_t i = 0; i < rl; i++) str_putc(qmask, 2);
            free(r);
            continue;
        }
        if (*p == '$' && p[1] == '(') {
            int depth = 1; p += 2;
            const char *st = p;
            while (*p) {
                if (*p == '(') depth++;
                else if (*p == ')') { depth--; if (!depth) break; }
                p++;
            }
            char *body = xstrndup(st, p - st);
            char *r = capture_subshell(body, 1);
            free(body);
            if (r) {
                size_t rl = strlen(r);
                str_putn(cur, r, rl);
                for (size_t i = 0; i < rl; i++) str_putc(qmask, 2);
                free(r);
            }
            if (*p == ')') p++;
            continue;
        }
        if (*p == '$' && p[1] == '{') {
            p += 2;
            int depth = 1;
            const char *st = p;
            while (*p && depth) {
                if (*p == '{') depth++;
                else if (*p == '}') depth--;
                if (depth) p++;
            }
            char *body = xstrndup(st, p - st);
            char *r = expand_param_body(body);
            free(body);
            if (r) {
                size_t rl = strlen(r);
                str_putn(cur, r, rl);
                for (size_t i = 0; i < rl; i++) str_putc(qmask, 2);
                free(r);
            }
            if (*p == '}') p++;
            continue;
        }
        if (*p == '$' && p[1] == '[') {
            p += 2;
            const char *st = p;
            while (*p && *p != ']') p++;
            char *body = xstrndup(st, p - st);
            char *r = xmalloc(32);
            snprintf(r, 32, "%lld", arith_eval(body));
            free(body);
            size_t rl = strlen(r);
            str_putn(cur, r, rl);
            for (size_t i = 0; i < rl; i++) str_putc(qmask, 2);
            free(r);
            if (*p == ']') p++;
            continue;
        }
        if (*p == '$' && (isalpha((unsigned char)p[1]) || p[1] == '_')) {
            p++;
            const char *st = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            char *nm = xstrndup(st, p - st);
            const char *v = var_get(nm);
            if (v) {
                size_t vl = strlen(v);
                str_putn(cur, v, vl);
                for (size_t i = 0; i < vl; i++) str_putc(qmask, 2);
            }
            free(nm);
            continue;
        }
        if (*p == '$' && isdigit((unsigned char)p[1])) {
            p++;
            const char *st = p;
            while (isdigit((unsigned char)*p)) p++;
            char *nm = xstrndup(st, p - st);
            char *v = positional_param(atoi(nm));
            free(nm);
            if (v) {
                size_t vl = strlen(v);
                str_putn(cur, v, vl);
                for (size_t i = 0; i < vl; i++) str_putc(qmask, 2);
                free(v);
            }
            continue;
        }
        if (*p == '$' && strchr("?#@*!$-0", p[1])) {
            char *v = special_param(p[1]);
            if (v) {
                size_t vl = strlen(v);
                for (size_t i = 0; i < vl; i++) {
                    if (v[i] == 1) {   /* field separator marker from $@/$* */
                        str_putc(cur, ' ');
                        str_putc(qmask, 2);
                    } else {
                        str_putc(cur, v[i]);
                        str_putc(qmask, 2);
                    }
                }
                free(v);
            }
            p += 2;
            continue;
        }
        if (*p == '`') {
            p++;
            const char *st = p;
            while (*p && *p != '`') p++;
            char *body = xstrndup(st, p - st);
            char *r = capture_subshell(body, 1);
            free(body);
            if (r) {
                size_t rl = strlen(r);
                str_putn(cur, r, rl);
                for (size_t i = 0; i < rl; i++) str_putc(qmask, 2);
                free(r);
            }
            if (*p == '`') p++;
            continue;
        }
        str_putc(cur, *p);
        str_putc(qmask, qmode == 1 ? 1 : 0);
        p++;
    }
}

/* split cur on IFS where the mask allows it; push fields into out */
static void split_and_push(Str *cur, Str *qmask, int do_split, int do_glob, Vec *out) {
    if (!cur->len) { return; }
    if (!do_split) {
        glob_one(cur->buf, qmask->buf, do_glob, out);
        return;
    }
    size_t i = 0;
    while (i < cur->len) {
        while (i < cur->len && (qmask->buf[i] == 1 || !is_ifs(cur->buf[i]))) {
            if (qmask->buf[i] == 2 && is_ifs(cur->buf[i])) break;
            i++;
        }
        if (i >= cur->len) break;
        /* separator: only unquoted IFS from expansion counts */
        i++;
    }
    /* simpler: rescan with fields */
    size_t start = 0;
    int saw_any = 0;
    for (i = 0; i <= cur->len; i++) {
        int is_sep = (i < cur->len) && qmask->buf[i] == 2 && is_ifs(cur->buf[i]);
        if (is_sep || i == cur->len) {
            if (i > start || i == cur->len) {
                if (i > start) {
                    char *field = xstrndup(cur->buf + start, i - start);
                    char *fm = xstrndup(qmask->buf + start, i - start);
                    glob_one(field, fm, do_glob, out);
                    free(field); free(fm);
                }
                saw_any = 1;
            }
            start = i + 1;
        }
    }
    if (!saw_any) glob_one(cur->buf, qmask->buf, do_glob, out);
}

void expand_str(const char *s, Vec *out, int flags) {
    Str cur, qmask;
    str_init(&cur); str_init(&qmask);
    expand_into(s, 0, &cur, &qmask);
    split_and_push(&cur, &qmask, (flags & EX_SPLIT) != 0, (flags & EX_GLOB) != 0, out);
    str_free(&cur); str_free(&qmask);
}

void expand_word(Word *w, Vec *out, int flags) {
    Str cur, qmask;
    str_init(&cur); str_init(&qmask);
    for (int i = 0; i < w->n; i++)
        expand_into(w->s[i].text, w->s[i].q, &cur, &qmask);
    /* tilde expansion on the unquoted result: leading ~ or ~user */
    if (cur.len && cur.buf[0] == '~' && (!qmask.len || qmask.buf[0] == 0)) {
        const char *rest = cur.buf + 1;
        const char *slash = strchr(rest, '/');
        size_t ulen = slash ? (size_t)(slash - rest) : strlen(rest);
        const char *home = NULL;
        if (ulen == 0) home = var_get("HOME");
        else {
            char *user = xstrndup(rest, ulen);
            struct passwd *pw = getpwnam(user);
            home = pw ? pw->pw_dir : NULL;
            free(user);
        }
        if (home) {
            Str tmp; str_init(&tmp);
            str_puts(&tmp, home);
            str_puts(&tmp, slash ? slash : "");
            Str qm; str_init(&qm);
            for (size_t k = 0; k < tmp.len; k++) str_putc(&qm, 1);
            str_free(&cur); str_free(&qmask);
            cur = tmp; qmask = qm;
        }
    }
    split_and_push(&cur, &qmask, (flags & EX_SPLIT) != 0, (flags & EX_GLOB) != 0, out);
    str_free(&cur); str_free(&qmask);
}

/* expand a single raw word to one string (no splitting); NULL if empty */
char *do_word_expansion(const char *raw, int allow_split_and_glob, Vec *out) {
    Vec v; vec_init(&v);
    expand_str(raw, &v, allow_split_and_glob ? (EX_SPLIT | EX_GLOB) : 0);
    if (v.len == 0) { vec_free(&v); return NULL; }
    Str s; str_init(&s);
    for (int i = 0; i < v.len; i++) {
        if (i) str_putc(&s, ' ');
        str_puts(&s, (char *)v.data[i]);
    }
    char *r = str_done(&s);
    if (out)
        for (int i = 0; i < v.len; i++) vec_push(out, v.data[i]);
    else
        vec_free(&v);
    if (out) vec_free(&v);
    return r;
}

int test_file_op(int op, const char *path) {
    struct stat st;
    switch (op) {
    case 'e': return path && access(path, F_OK) == 0;
    case 'f': return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
    case 'd': return is_directory(path);
    case 'r': return path && access(path, R_OK) == 0;
    case 'w': return path && access(path, W_OK) == 0;
    case 'x': return path && access(path, X_OK) == 0;
    case 's': return path && stat(path, &st) == 0 && st.st_size > 0;
    default:  return 0;
    }
}

int is_executable(const char *p) { return access(p, X_OK) == 0; }

/* glob one field: if any unquoted glob char is present, match the filesystem,
 * otherwise emit the field verbatim. */
void glob_one(const char *field, const char *qmask, int do_glob, Vec *out) {
    if (!do_glob || g_opt_noglob) { vec_push(out, xstrdup(field)); return; }
    int wild = 0;
    for (const char *p = field; *p; p++) {
        if (*p == '\\' && p[1]) { p++; continue; }
        if (qmask && qmask[p - field] == 1) continue;   /* quoted char */
        if (*p == '*' || *p == '?' || *p == '[') { wild = 1; break; }
    }
    if (!wild) { vec_push(out, xstrdup(field)); return; }

    Str cur; str_init(&cur);
    /* strip backslashes from unquoted glob metacharacters */
    for (const char *p = field; *p; p++) {
        if (*p == '\\' && p[1] && qmask && qmask[p - field] != 1) {
            str_putc(&cur, p[1]); p++;
            continue;
        }
        if (*p == '\\' && p[1]) { p++; str_putc(&cur, *p); continue; }
        str_putc(&cur, *p);
    }
    Glob g = {0};
    if (glob_pattern(cur.buf, &g) && g.count > 0) {
        for (size_t i = 0; i < g.count; i++)
            vec_push(out, g.paths[i]);
        g.count = 0;   /* ownership transferred */
    } else {
        vec_push(out, xstrdup(cur.buf));   /* nomatch: pattern stays literal */
    }
    glob_free(&g);
    str_free(&cur);
}

