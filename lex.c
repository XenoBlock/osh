/* lex.c - tokenizer with quote tracking, operators, here-documents.
 *
 * Words are stored as a list of segments, each tagged with its quote context
 * (0 plain, 1 double, 2 single) so expansion knows where word splitting and
 * globbing are allowed. */
#include "osh.h"
#include <ctype.h>

/* ---------------- readers ---------------- */
void reader_init_buf(Reader *r, const char *s) {
    memset(r, 0, sizeof *r);
    r->buf = xstrdup(s);
    r->len = strlen(s);
    r->no_more = 1;
}

void reader_init_file(Reader *r, FILE *f) {
    memset(r, 0, sizeof *r);
    r->data = f;
    r->no_more = 1;
}

void reader_init_tty(Reader *r) {
    memset(r, 0, sizeof *r);
    extern char *edit_getline(const char *prompt);
    r->fill = (char *(*)(Reader *, const char *))edit_getline;
}

/* append the next physical line to dst; 1 on success, 0 at EOF */
int reader_getline(Reader *r, const char *prompt, Str *dst) {
    if (r->buf) {
        if (r->pos >= r->len) return 0;
        while (r->pos < r->len && r->buf[r->pos] != '\n')
            str_putc(dst, r->buf[r->pos++]);
        if (r->pos < r->len) r->pos++;      /* consume the newline */
        return 1;
    }
    if (r->data) {
        FILE *f = r->data;
        int c = getc(f);
        if (c == EOF) return 0;
        while (c != EOF && c != '\n') { str_putc(dst, (char)c); c = getc(f); }
        return 1;
    }
    if (r->fill) {
        char *line = r->fill(r, prompt);
        if (!line) return 0;
        str_puts(dst, line);
        free(line);
        return 1;
    }
    return 0;
}

/* ---------------- words ---------------- */
void word_free(Word *w) {
    for (int i = 0; i < w->n; i++) free(w->s[i].text);
    free(w->s);
    w->s = 0; w->n = 0;
}

void word_add(Word *w, const char *t, int q) {
    if (w->n + 1 > w->cap) {
        w->cap = w->cap ? w->cap * 2 : 8;
        w->s = xrealloc(w->s, w->cap * sizeof(Seg));
    }
    w->s[w->n].text = xstrdup(t);
    w->s[w->n].q = q;
    w->n++;
}

char *word_raw(Word *w) {
    Str s; str_init(&s);
    for (int i = 0; i < w->n; i++) str_puts(&s, w->s[i].text);
    return str_done(&s);
}

int word_unquoted(Word *w) {
    for (int i = 0; i < w->n; i++) if (w->s[i].q) return 0;
    return 1;
}

void token_free(Token *t) {
    word_free(&t->word);
    memset(t, 0, sizeof *t);
    t->fd = -1;
}

/* ---------------- character stream ---------------- */
static void lx_buf_grow(Lexer *lx, size_t extra) {
    if (lx->len + extra + 1 <= lx->cap) return;
    size_t want = lx->cap ? lx->cap : 256;
    while (want < lx->len + extra + 1) want *= 2;
    lx->buf = xrealloc(lx->buf, want);
    lx->cap = want;
}

void lex_init(Lexer *lx, Reader *r) {
    memset(lx, 0, sizeof *lx);
    lx->r = r;
    lx->tok.fd = -1;
    lx->peek.fd = -1;
}

/* pull one physical line into the buffer */
int lex_fill(Lexer *lx) {
    if (lx->eof) return 0;
    Str line; str_init(&line);
    int ok = reader_getline(lx->r, "> ", &line);
    if (!ok) { str_free(&line); lx->eof = 1; return 0; }
    lx_buf_grow(lx, line.len + 2);
    /* keep every logical line newline-terminated */
    if (lx->len > 0 && lx->buf[lx->len - 1] != '\n')
        lx->buf[lx->len++] = '\n';
    memcpy(lx->buf + lx->len, line.buf, line.len);
    lx->len += line.len;
    lx->buf[lx->len++] = '\n';
    lx->buf[lx->len] = 0;
    lx->line_no++;
    str_free(&line);
    return 1;
}

static int lex_getc(Lexer *lx) {
    if (lx->pos < lx->len) return (unsigned char)lx->buf[lx->pos++];
    if (lex_fill(lx) && lx->pos < lx->len) return (unsigned char)lx->buf[lx->pos++];
    return -1;
}

static int lex_at(Lexer *lx, size_t off) {
    while (lx->len <= lx->pos + off)
        if (!lex_fill(lx)) return -1;
    return (unsigned char)lx->buf[lx->pos + off];
}

static int lex_peekc(Lexer *lx) { return lex_at(lx, 0); }
static int lex_peekc2(Lexer *lx) { return lex_at(lx, 1); }

/* copy a balanced $(...), ${...} or `...` verbatim into out.
 * depth0 accounts for the extra paren of $(( )) */
static void read_balanced(Lexer *lx, Str *out, char open, char close, int depth0) {
    int depth = depth0;
    for (;;) {
        int c = lex_getc(lx);
        if (c < 0) break;
        str_putc(out, (char)c);
        if (c == open) depth++;
        else if (c == close) { depth--; if (!depth) break; }
    }
}

/* read a quoted section into out, keeping escapes for the expander */
static int read_quote(Lexer *lx, char q, Str *out) {
    for (;;) {
        int c = lex_getc(lx);
        if (c < 0) return 0;
        if (c == q) return 1;
        if (q == '"' && c == '\\') {
            int n = lex_peekc(lx);
            if (n == '"' || n == '\\' || n == '$' || n == '`') {
                str_putc(out, '\\');
                str_putc(out, (char)lex_getc(lx));
                continue;
            }
            if (n == '\n') { lex_getc(lx); continue; }
            str_putc(out, '\\');
            str_putc(out, (char)lex_getc(lx));
            continue;
        }
        str_putc(out, (char)c);
    }
}

/* read a here-document body; terminator alone on a line ends it */
char *lex_read_heredoc(Lexer *lx, const char *term, int strip_tabs) {
    Str body; str_init(&body);
    Str line; str_init(&line);
    for (;;) {
        str_clear(&line);
        if (!lex_getline(lx, &line)) break;
        const char *s = line.buf ? line.buf : "";
        if (strip_tabs) while (*s == '\t') s++;
        if (strcmp(s, term) == 0) break;
        str_puts(&body, s);
        str_putc(&body, '\n');
    }
    str_free(&line);
    return str_done(&body);
}

/* read one word; returns 1 if any character was consumed */
static int read_word(Lexer *lx, Token *tok) {
    Str seg; str_init(&seg);
    int qcur = 0, got = 0;
    for (;;) {
        int c = lex_peekc(lx);
        if (c < 0) break;
        if (qcur == 0 && (c == ' ' || c == '\t' || c == '\n' ||
                          c == '|' || c == '&' || c == ';' ||
                          c == '(' || c == ')' || c == '<' || c == '>'))
            break;
        if (qcur == 0 && c == '#') {
            while (c >= 0 && c != '\n') c = lex_getc(lx);
            break;
        }
        lex_getc(lx);
        got = 1;
        if (c == '\'' || c == '"') {
            if (seg.len) { word_add(&tok->word, seg.buf, qcur); str_clear(&seg); }
            read_quote(lx, (char)c, &seg);
            word_add(&tok->word, seg.buf, c == '"' ? 1 : 2);
            str_clear(&seg);
            qcur = 0;               /* quote consumed its own terminator */
            continue;
        }
        if (c == '$' && lex_peekc(lx) == '(') {
            lex_getc(lx);                            /* consume '(' */
            int extra = 0;
            if (lex_peekc(lx) == '(') {              /* $(( )) arithmetic */
                lex_getc(lx); extra = 1;
                Str body; str_init(&body);
                str_puts(&body, "$((");
                read_balanced(lx, &body, '(', ')', 2);
                str_puts(&seg, body.buf);
                str_free(&body);
                (void)extra;
                continue;
            }
            Str body; str_init(&body);
            str_putc(&body, '$'); str_putc(&body, '(');
            read_balanced(lx, &body, '(', ')', 1);
            str_puts(&seg, body.buf);
            str_free(&body);
            continue;
        }
        if (c == '$' && lex_peekc(lx) == '{') {
            lex_getc(lx);
            Str body; str_init(&body);
            str_putc(&body, '$'); str_putc(&body, '{');
            read_balanced(lx, &body, '{', '}', 1);
            str_puts(&seg, body.buf);
            str_free(&body);
            continue;
        }
        if (c == '`') {
            Str body; str_init(&body);
            str_putc(&body, '`');
            for (;;) {
                int n = lex_getc(lx);
                if (n < 0) break;
                str_putc(&body, (char)n);
                if (n == '`') break;
            }
            str_puts(&seg, body.buf);
            str_free(&body);
            continue;
        }
        if (c == '\\') {
            int n = lex_getc(lx);
            if (n == '\n') continue;
            if (n < 0) break;
            str_putc(&seg, '\\');
            str_putc(&seg, (char)n);
            continue;
        }
        str_putc(&seg, (char)c);
    }
    if (seg.len) word_add(&tok->word, seg.buf, qcur);
    str_free(&seg);
    return got;
}

static const char *operators[] = {
    "&&", "||", ";;", "<<<", ">>", ">&", "<&", ">|", "<<-", "<<", ">>",
    ">", "<", "|", "&", ";", "(", ")", "\n", NULL
};

void lex_next(Lexer *lx) {
    token_free(&lx->tok);
    Token *tok = &lx->tok;
    tok->fd = -1;
    for (;;) {
        int c = lex_peekc(lx);
        if (c < 0) { tok->type = T_EOF; return; }
        if (c == ' ' || c == '\t') { lex_getc(lx); continue; }
        if (c == '\\' && lex_peekc2(lx) == '\n') { lex_getc(lx); lex_getc(lx); continue; }
        if (c == '#') { while (c >= 0 && c != '\n') c = lex_getc(lx); continue; }
        break;
    }
    /* optional leading fd number: 2>&1, 2>file, 1>&2 */
    while (!lx->in_heredoc && lx->len < lx->pos + 4) if (!lex_fill(lx)) break;
    int lead_fd = -1;
    if (lx->pos < lx->len && isdigit((unsigned char)lx->buf[lx->pos]) &&
        lx->pos + 1 < lx->len &&
        (lx->buf[lx->pos + 1] == '>' || lx->buf[lx->pos + 1] == '<')) {
        lead_fd = lx->buf[lx->pos] - '0';
        lx->pos++;
    }
    for (int i = 0; operators[i]; i++) {
        size_t ol = strlen(operators[i]);
        if (lx->pos + ol <= lx->len &&
            memcmp(lx->buf + lx->pos, operators[i], ol) == 0) {
            memcpy(tok->op, operators[i], ol);
            tok->op[ol] = 0;
            lx->pos += ol;
            const char *o = operators[i];
            tok->type = T_WORD;
            if (!strcmp(o, "|")) tok->type = T_PIPE;
            else if (!strcmp(o, "&&")) tok->type = T_AND;
            else if (!strcmp(o, "||")) tok->type = T_OR;
            else if (!strcmp(o, ";") || !strcmp(o, ";;")) tok->type = T_SEMI;
            else if (!strcmp(o, "&")) tok->type = T_AMP;
            else if (!strcmp(o, "(")) tok->type = T_LP;
            else if (!strcmp(o, ")")) tok->type = T_RP;
            else if (!strcmp(o, "\n")) tok->type = T_NEWLINE;
            else {
                tok->type = T_REDIR;
                tok->fd = (o[0] == '<' && strcmp(o, "<&") != 0) ? 0 : 1;
                if (!strncmp(o, ">&", 2) || !strncmp(o, "<&", 2)) tok->fd = -1;
                if (lead_fd >= 0) tok->fd = lead_fd;
                if (!strcmp(o, "<<") || !strcmp(o, "<<-")) {
                    /* consume the delimiter and body now so the parser
                       never sees the body as ordinary words */
                    int strip = (o[2] == '-');
                    Str delim; str_init(&delim);
                    while (lx->pos < lx->len && isspace((unsigned char)lx->buf[lx->pos]) &&
                           lx->buf[lx->pos] != '\n') lx->pos++;
                    size_t doff = lx->pos;      /* delimiter start, as an offset */
                    while (lx->pos < lx->len && lx->buf[lx->pos] != '\n' &&
                           !strchr("|&;()<>	 ", lx->buf[lx->pos]))
                        str_putc(&delim, lx->buf[lx->pos++]);
                    /* The rest of the command line (`| cat` in `<<E | cat`)
                       still belongs to this command; the body starts on the
                       next line and may already be buffered by the lookahead
                       above. Read the body from there, then drop it so the
                       parser only sees the tail. */
                    size_t tail = lx->pos, nl = lx->pos;
                    while (nl < lx->len && lx->buf[nl] != '\n') nl++;
                    lx->pos = (nl < lx->len) ? nl + 1 : nl;
                    lx->eof = 0;       /* the lexer may keep reading the input */
                    lx->in_heredoc = 1;
                    tok->heredoc = lex_read_heredoc(lx, delim.buf ? delim.buf : "", strip);
                    lx->in_heredoc = 0;
                    /* drop the consumed body, keeping the tail text */
                    if (lx->pos > nl) {
                        memmove(lx->buf + nl, lx->buf + lx->pos, lx->len - lx->pos);
                        lx->len -= lx->pos - nl;
                    }
                    lx->pos = tail;    /* replay the tail for the parser */
                    /* emit the delimiter as a word so the parser can see it */
                    if (doff < lx->len || delim.buf) {
                        word_add(&tok->word, delim.buf, 0);
                        tok->type = T_REDIR;   /* keep type; word is the delimiter */
                    }
                    str_free(&delim);
                }
            }
            return;
        }
    }
    read_word(lx, tok);
    tok->type = T_WORD;
}

/* consume the current token, filling in the next one */
void advance_token(Lexer *lx) {
    if (lx->have_peek) { lx->tok = lx->peek; lx->have_peek = 0; }
    else lex_next(lx);
}

/* read one raw line for a heredoc body: first drain the already-lexed
   buffer, then read fresh lines from the reader */
int lex_getline(Lexer *lx, Str *dst) {
    while (lx->pos < lx->len && lx->buf[lx->pos] != '\n')
        str_putc(dst, lx->buf[lx->pos++]);
    if (lx->pos < lx->len) { lx->pos++; return 1; }   /* consumed a full line */
    return reader_getline(lx->r, "> ", dst);
}

/* true if the character after the current word is `(` (function definition).
 * Inspects the raw buffer so the current token stays intact. */
int lx_looks_like_fn(Lexer *lx) {
    size_t i = lx->pos;
    for (;;) {
        while (i < lx->len && (lx->buf[i] == ' ' || lx->buf[i] == '\t')) i++;
        if (i < lx->len) break;
        if (!lex_fill(lx)) return 0;
        i = lx->pos;
    }
    return lx->buf[i] == '(';
}

void lex_peek(Lexer *lx) {
    if (lx->have_peek) return;
    Token saved = lx->tok;
    lex_next(lx);
    lx->peek = lx->tok;
    lx->tok = saved;
    lx->have_peek = 1;
}
