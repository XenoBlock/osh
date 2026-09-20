/* parse.c - recursive descent parser producing an AST.
 *
 * Grammar (POSIX-ish subset):
 *   line    := list EOF
 *   list    := andor (sep andor)*
 *   andor   := pipeline (('&&'|'||') pipeline)*
 *   pipeline:= command ('|' command)*
 *   command := simple | subshell | group | if | while | until | for | case
 *   simple  := (assign | redir | word)+
 */
#include "osh.h"
#include <ctype.h>

static Node *new_node(int kind) {
    Node *n = xmalloc(sizeof(Node));
    memset(n, 0, sizeof *n);
    n->kind = kind;
    return n;
}

void node_free(Node *n) {
    if (!n) return;
    node_free(n->a); node_free(n->b); node_free(n->c);
    for (int i = 0; i < n->nargs; i++) word_free(&n->args[i]);
    free(n->args);
    for (int i = 0; i < n->nassigns; i++) free(n->assigns[i]);
    free(n->assigns);
    for (int i = 0; i < n->nredirs; i++) {
        word_free(&n->redirs[i].target);
        free(n->redirs[i].heredoc);
    }
    free(n->redirs);
    free(n->var);
    free(n);
}

static void node_add_arg(Node *n, Word *w) {
    /* deep copy: the lexer's word is reused for the next token */
    n->args = xrealloc(n->args, (n->nargs + 1) * sizeof(Word));
    Word *dst = &n->args[n->nargs++];
    dst->s = xmalloc((w->n ? w->n : 1) * sizeof(Seg));
    dst->n = w->n;
    dst->cap = w->n;
    for (int i = 0; i < w->n; i++) {
        dst->s[i].text = xstrdup(w->s[i].text);
        dst->s[i].q = w->s[i].q;
    }
}

static void node_add_assign(Node *n, char *raw) {
    n->assigns = xrealloc(n->assigns, (n->nassigns + 1) * sizeof(char *));
    n->assigns[n->nassigns++] = raw;
}

static void node_add_redir(Node *n, Redir *r) {
    n->redirs = xrealloc(n->redirs, (n->nredirs + 1) * sizeof(Redir));
    n->redirs[n->nredirs++] = *r;
}

static int tok_is(Lexer *lx, TokType t) { return lx->tok.type == t; }

static void advance(Lexer *lx) { advance_token(lx); }

/* map operator text to a redirection type */
static int redir_type(const char *op) {
    if (!strcmp(op, "<"))  return R_IN;
    if (!strcmp(op, ">"))  return R_OUT;
    if (!strcmp(op, ">>")) return R_APP;
    if (!strcmp(op, "2>")) return R_ERR;
    if (!strcmp(op, ">&")) return R_DUPOUT;
    if (!strcmp(op, "<&")) return R_DUPIN;
    if (!strcmp(op, ">|")) return R_CLOBBER;
    if (!strcmp(op, "<<<")) return R_HERESTR;
    if (!strcmp(op, "<<")) return R_HEREDOC;
    if (!strcmp(op, "<<-")) return R_DHEREDOC;
    if (!strcmp(op, "<>")) return R_INOUT;
    return R_OUT;
}

static int parse_redir_target(Lexer *lx, Node *n, const char *op, int fd) {
    Redir r;
    memset(&r, 0, sizeof r);
    r.fd = fd;
    r.type = redir_type(op);
    if (r.type == R_HEREDOC || r.type == R_DHEREDOC) {
        /* the lexer stored the delimiter in word and the body in heredoc */
        if (!lx->tok.word.n) return -1;
        int quoted = !word_unquoted(&lx->tok.word);
        r.heredoc = lx->tok.heredoc ? lx->tok.heredoc : xstrdup("");
        lx->tok.heredoc = NULL;
        word_free(&lx->tok.word);
        memset(&lx->tok.word, 0, sizeof lx->tok.word);
        advance(lx);
        if (!quoted) {
            /* expansion happens at parse time for unquoted terminators */
            Vec v; vec_init(&v);
            expand_str(r.heredoc, &v, 0);
            if (v.len) {
                Str j; str_init(&j);
                for (int i = 0; i < v.len; i++) {
                    if (i) str_putc(&j, ' ');
                    str_puts(&j, (char *)v.data[i]);
                }
                free(r.heredoc);
                r.heredoc = str_done(&j);
            }
            vec_free(&v);
        }
        node_add_redir(n, &r);
        return 0;
    }
    /* normal redirect: the target is the next token */
    advance(lx);
    if (!tok_is(lx, T_WORD)) return -1;
    r.target = lx->tok.word;
    memset(&lx->tok.word, 0, sizeof lx->tok.word);
    node_add_redir(n, &r);
    return 0;
}

/* true if a word looks like NAME=... (assignment) */
static int word_is_assign(Word *w) {
    if (!w->n) return 0;
    const char *s = w->s[0].text;
    if (!(isalpha((unsigned char)*s) || *s == '_')) return 0;
    for (const char *p = s; *p; p++) {
        if (*p == '=') return p != s;
        if (!isalnum((unsigned char)*p) && *p != '_') return 0;
    }
    return 0;
}


static Node *parse_pipeline(Lexer *lx);
static Node *parse_andor(Lexer *lx);
static Node *parse_list(Lexer *lx);
static Node *parse_command(Lexer *lx);

/* ( ... ) subshell or { ...; } group */
static int tok_is_close_brace(Lexer *lx) {
    if (!tok_is(lx, T_WORD)) return 0;
    char *w = word_raw(&lx->tok.word);
    int r = w && !strcmp(w, "}");
    free(w);
    return r;
}

static Node *parse_subor_group(Lexer *lx, int group) {
    Node *n = new_node(group ? N_GROUP : N_SUB);
    advance(lx);                                   /* consume ( or { */
    n->a = parse_list(lx);
    if (group) {
        if (tok_is_close_brace(lx)) advance(lx);
        else if (!tok_is(lx, T_EOF)) fprintf(stderr, "osh: syntax: `}' expected\n");
    } else {
        if (tok_is(lx, T_RP)) advance(lx);
        else if (!tok_is(lx, T_EOF)) fprintf(stderr, "osh: syntax: `)' expected\n");
    }
    return n;
}

static Node *parse_if(Lexer *lx) {
    Node *n = new_node(N_IF);
    advance(lx);
    n->a = parse_list(lx);
    if (!tok_is(lx, T_WORD) || strcmp(word_raw(&lx->tok.word), "then")) {
        fprintf(stderr, "osh: syntax: `then' expected\n");
        node_free(n); return NULL;
    }
    advance(lx);
    n->b = parse_list(lx);
    Node *else_branch = NULL;
    if (tok_is(lx, T_WORD)) {
        char *w = word_raw(&lx->tok.word);
        if (!strcmp(w, "elif")) {
            else_branch = parse_if(lx);            /* elif is a nested if */
        } else if (!strcmp(w, "else")) {
            advance(lx);
            else_branch = parse_list(lx);
            if (!tok_is(lx, T_WORD) || strcmp(word_raw(&lx->tok.word), "fi")) {
                fprintf(stderr, "osh: syntax: `fi' expected\n");
            } else advance(lx);
        } else if (!strcmp(w, "fi")) {
            advance(lx);
        } else {
            fprintf(stderr, "osh: syntax: `fi' expected\n");
        }
        free(w);
    }
    n->c = else_branch;
    return n;
}

static Node *parse_loop(Lexer *lx, int kind, const char *mid, const char *end) {
    Node *n = new_node(kind);
    advance(lx);
    n->a = parse_list(lx);
    if (!tok_is(lx, T_WORD) || strcmp(word_raw(&lx->tok.word), mid)) {
        fprintf(stderr, "osh: syntax: `%s' expected\n", mid);
        node_free(n); return NULL;
    }
    advance(lx);
    n->b = parse_list(lx);
    if (!tok_is(lx, T_WORD) || strcmp(word_raw(&lx->tok.word), end)) {
        fprintf(stderr, "osh: syntax: `%s' expected\n", end);
    } else advance(lx);
    return n;
}

static Node *parse_for(Lexer *lx) {
    Node *n = new_node(N_FOR);
    advance(lx);
    if (!tok_is(lx, T_WORD)) { fprintf(stderr, "osh: syntax: `for' needs a name\n"); node_free(n); return NULL; }
    char *name = word_raw(&lx->tok.word);
    n->var = name;
    advance(lx);
    /* optional `in word...` */
    if (tok_is(lx, T_WORD) && !strcmp(word_raw(&lx->tok.word), "in")) {
        advance(lx);
        while (tok_is(lx, T_WORD) && strcmp(word_raw(&lx->tok.word), "do")) {
            node_add_arg(n, &lx->tok.word);
            memset(&lx->tok.word, 0, sizeof lx->tok.word);
            advance(lx);
        }
        if (tok_is(lx, T_SEMI)) advance(lx);
    }
    if (!tok_is(lx, T_WORD) || strcmp(word_raw(&lx->tok.word), "do")) {
        fprintf(stderr, "osh: syntax: `do' expected\n");
        node_free(n); return NULL;
    }
    advance(lx);
    n->a = parse_list(lx);
    if (!tok_is(lx, T_WORD) || strcmp(word_raw(&lx->tok.word), "done")) {
        fprintf(stderr, "osh: syntax: `done' expected\n");
    } else advance(lx);
    n->nargs_set = n->nargs > 0;
    return n;
}

static Node *parse_case(Lexer *lx) {
    Node *n = new_node(N_CASE);
    advance(lx);
    if (!tok_is(lx, T_WORD)) { fprintf(stderr, "osh: syntax: `case' needs a word\n"); node_free(n); return NULL; }
    node_add_arg(n, &lx->tok.word);
    memset(&lx->tok.word, 0, sizeof lx->tok.word);
    advance(lx);
    if (!tok_is(lx, T_WORD) || strcmp(word_raw(&lx->tok.word), "in")) {
        fprintf(stderr, "osh: syntax: `in' expected\n");
        node_free(n); return NULL;
    }
    advance(lx);
    n->a = parse_case_list(lx);
    if (!tok_is(lx, T_WORD) || strcmp(word_raw(&lx->tok.word), "esac")) {
        fprintf(stderr, "osh: syntax: `esac' expected\n");
    } else advance(lx);
    return n;
}

Node *parse_case_list(Lexer *lx) {
    /* each branch: (pattern) list ;; */
    Node *head = NULL, *tail = NULL;
    while (tok_is(lx, T_WORD) && strcmp(word_raw(&lx->tok.word), "esac")) {
        Node *b = new_node(N_NONE);
        node_add_arg(b, &lx->tok.word);
        memset(&lx->tok.word, 0, sizeof lx->tok.word);
        advance(lx);
        while (tok_is(lx, T_PIPE)) {
            advance(lx);
            if (!tok_is(lx, T_WORD)) break;
            node_add_arg(b, &lx->tok.word);
            memset(&lx->tok.word, 0, sizeof lx->tok.word);
            advance(lx);
        }
        if (!tok_is(lx, T_RP)) {
            fprintf(stderr, "osh: syntax: `)' expected in case\n");
            node_free(b); break;
        }
        advance(lx);
        b->a = parse_list(lx);
        if (tok_is(lx, T_SEMI) && !strcmp(lx->tok.op, ";;")) advance(lx);
        if (head) { tail->b = b; tail = b; } else { head = tail = b; }
    }
    return head;
}

/* a simple command: assignments, words and redirections */
static Node *parse_simple(Lexer *lx) {
    Node *n = new_node(N_CMD);
    int seen_word = 0;
    for (;;) {
        if (tok_is(lx, T_WORD)) {
            if (!seen_word && word_is_assign(&lx->tok.word)) {
                char *raw = word_raw(&lx->tok.word);
                node_add_assign(n, raw);
                word_free(&lx->tok.word);
                memset(&lx->tok.word, 0, sizeof lx->tok.word);
            } else {
                node_add_arg(n, &lx->tok.word);
                memset(&lx->tok.word, 0, sizeof lx->tok.word);
                seen_word = 1;
            }
            advance(lx);
            continue;
        }
        if (tok_is(lx, T_REDIR)) {
            char op[8];
            memcpy(op, lx->tok.op, sizeof op);
            int fd = lx->tok.fd;
            if (parse_redir_target(lx, n, op, fd) < 0) {
                fprintf(stderr, "osh: syntax: missing redirection target\n");
                node_free(n); return NULL;
            }
            continue;
        }
        break;
    }
    if (!n->nargs && !n->nassigns && !n->nredirs) { node_free(n); return NULL; }
    return n;
}

static Node *parse_function(Lexer *lx) {
    /* NAME () compound-command */
    char *name = word_raw(&lx->tok.word);
    advance(lx);                            /* consume the name */
    if (!tok_is(lx, T_LP)) { free(name); return NULL; }
    advance(lx);                            /* ( */
    if (!tok_is(lx, T_RP)) { free(name); return NULL; }
    advance(lx);                            /* ) */
    Node *body = parse_command(lx);
    if (!body) { free(name); return NULL; }
    Str src; str_init(&src);
    node_to_source(body, &src);
    map_put(&g_funs, name, src.buf);
    free(name);
    str_free(&src);
    node_free(body);
    Node *n = new_node(N_NONE);
    return n;
}

static Node *parse_command(Lexer *lx) {
    if (tok_is(lx, T_LP)) return parse_subor_group(lx, 0);
    if (tok_is(lx, T_WORD)) {
        char *w = word_raw(&lx->tok.word);
        Node *n = NULL;
        /* function definition: NAME ( ) ... */
        if (word_unquoted(&lx->tok.word) && lx_looks_like_fn(lx)) {
            free(w);
            return parse_function(lx);
        }
        if (!strcmp(w, "if"))        n = parse_if(lx);
        else if (!strcmp(w, "while")) n = parse_loop(lx, N_WHILE, "do", "done");
        else if (!strcmp(w, "until")) n = parse_loop(lx, N_UNTIL, "do", "done");
        else if (!strcmp(w, "for"))   n = parse_for(lx);
        else if (!strcmp(w, "case"))  n = parse_case(lx);
        else if (!strcmp(w, "{"))     n = parse_subor_group(lx, 1);
        else if (!strcmp(w, "!"))  { free(w); advance(lx); n = parse_pipeline(lx); return n; }
        free(w);
        if (n) {
            /* trailing redirections attach to the compound command */
            for (;;) {
                if (!tok_is(lx, T_REDIR)) break;
                char *op = xstrdup(lx->tok.op);
                int fd = lx->tok.fd;
                if (parse_redir_target(lx, n, op, fd) < 0) {
                    fprintf(stderr, "osh: syntax: missing redirection target\n");
                    free(op);
                    break;
                }
                free(op);
            }
            return n;
        }
    }
    return parse_simple(lx);
}

static Node *parse_pipeline(Lexer *lx) {
    Node *left = parse_command(lx);
    while (tok_is(lx, T_PIPE)) {
        advance(lx);
        Node *right = parse_command(lx);
        Node *p = new_node(N_PIPE);
        p->a = left; p->b = right;
        left = p;
    }
    return left;
}

static Node *parse_andor(Lexer *lx) {
    Node *left = parse_pipeline(lx);
    for (;;) {
        if (tok_is(lx, T_AND) || tok_is(lx, T_OR)) {
            int kind = tok_is(lx, T_AND) ? N_AND : N_OR;
            advance(lx);
            Node *right = parse_pipeline(lx);
            Node *n = new_node(kind);
            n->a = left; n->b = right;
            left = n;
            continue;
        }
        break;
    }
    return left;
}

static int is_terminator_word(Lexer *lx) {
    if (!tok_is(lx, T_WORD)) return 0;
    static const char *terms[] = {
        "then", "else", "elif", "fi", "do", "done", "esac", "}", NULL
    };
    char *w = word_raw(&lx->tok.word);
    for (int i = 0; terms[i]; i++)
        if (!strcmp(w, terms[i])) { free(w); return 1; }
    free(w);
    return 0;
}

static int is_list_end(Lexer *lx) {
    return tok_is(lx, T_EOF) || tok_is(lx, T_NEWLINE) || tok_is(lx, T_SEMI) ||
           tok_is(lx, T_AMP)  || tok_is(lx, T_RP) || is_terminator_word(lx);
}

static Node *parse_list(Lexer *lx) {
    Node *head = NULL, *tail = NULL;
    int bg = 0;
    for (;;) {
        if (is_list_end(lx)) {
            if (tok_is(lx, T_AMP)) { bg = 1; advance(lx); }
            else if (tok_is(lx, T_SEMI) || tok_is(lx, T_NEWLINE)) advance(lx);
            break;
        }
        Node *n = parse_andor(lx);
        if (!n) break;
        if (bg) { Node *g = new_node(N_BG); g->a = n; n = g; bg = 0; }
        if (head) {
            Node *seq = new_node(N_SEQ);
            seq->a = tail; seq->b = n;
            head = tail = seq;
        } else { head = tail = n; }
        if (tok_is(lx, T_AMP)) { bg = 1; advance(lx); }
        else if (tok_is(lx, T_SEMI) || tok_is(lx, T_NEWLINE)) {
            if (!strcmp(lx->tok.op, ";;")) break;   /* case branch terminator */
            advance(lx);
        }
        else if (!is_list_end(lx)) break;
    }
    return head;
}

Node *parse_line(Lexer *lx) {
    advance(lx);
    if (tok_is(lx, T_EOF) || tok_is(lx, T_NEWLINE)) return NULL;
    Node *n = parse_list(lx);
    return n;
}
