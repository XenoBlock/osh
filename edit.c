/* edit.c - interactive line editor: cursor movement, history, completion.
 *
 * Uses raw terminal mode on stdin. If stdin is not a terminal the editor
 * degrades to plain fgets() so that osh works in pipelines and scripts. */
#include "osh.h"
#include <termios.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <pwd.h>

static struct termios saved_termios;
static int raw_enabled = 0;
static int editor_ok = 0;

static char *hist[HIST_MAX];
static int hist_count = 0;
static int hist_pos = 0;      /* position while browsing */

static void hist_add(const char *line) {
    if (!line || !*line) return;
    if (hist_count && !strcmp(hist[hist_count - 1], line)) return;
    if (hist_count >= HIST_MAX) {
        free(hist[0]);
        for (int i = 1; i < hist_count; i++) hist[i - 1] = hist[i];
        hist_count--;
    }
    hist[hist_count++] = xstrdup(line);
}

void edit_add_history(const char *line) { hist_add(line); }

void history_print(void) {
    for (int i = 0; i < hist_count; i++) printf("%5d  %s\n", i + 1, hist[i]);
}

static char *hist_file_path(void) {
    Str s; str_init(&s);
    str_puts(&s, home_dir());
    str_putc(&s, '/');
    str_puts(&s, HIST_FILE);
    return str_done(&s);
}

void edit_load_history(void) {
    char *path = hist_file_path();
    int fd = open_user_file(path, 0);
    if (fd < 0) { free(path); return; }
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); free(path); return; }
    Str line; str_init(&line);
    int c;
    while ((c = getc(f)) != EOF) {
        if (c == '\n') {
            if (line.len) hist_add(line.buf);
            str_clear(&line);
        } else str_putc(&line, (char)c);
    }
    if (line.len) hist_add(line.buf);
    str_free(&line);
    fclose(f);
    free(path);
}

void edit_save_history(void) {
    char *path = hist_file_path();
    int fd = open_user_file(path, 1);
    if (fd < 0) { free(path); return; }
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); free(path); return; }
    int start = hist_count > HIST_MAX ? hist_count - HIST_MAX : 0;
    for (int i = start; i < hist_count; i++) {
        fputs(hist[i], f);
        fputc('\n', f);
    }
    fclose(f);
    free(path);
}

/* ---------------- terminal control ---------------- */
static void raw_on(void) {
    if (raw_enabled) return;
    if (tcgetattr(0, &saved_termios) != 0) return;
    struct termios t = saved_termios;
    t.c_lflag &= ~(ICANON | ECHO | ISIG);
    t.c_iflag &= ~(IXON | ICRNL);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &t) != 0) return;
    raw_enabled = 1;
}

static void raw_off(void) {
    if (!raw_enabled) return;
    tcsetattr(0, TCSANOW, &saved_termios);
    raw_enabled = 0;
}

void edit_init(void) {
    editor_ok = isatty(0) && isatty(1);
    edit_load_history();
    /* `exit` longjmps straight out of the interactive loop, so register the
       save here instead of only at the bottom of run_interactive(). */
    atexit(edit_save_history);
}

void edit_disable(void) { editor_ok = 0; }

/* ---------------- rendering ---------------- */
static void refresh(const char *prompt, const char *buf, int pos, int len) {
    /* redraw the line: \r, prompt, text, clear to end, place cursor */
    fputs("\r", stdout);
    fputs(prompt, stdout);
    fwrite(buf, 1, (size_t)len, stdout);
    fputs("\x1b[K", stdout);
    for (int i = len; i > pos; i--) fputs("\b", stdout);
    fflush(stdout);
}

/* ---------------- completion ---------------- */
static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* sort matches and drop duplicates (frees the discarded strings) */
static void vec_sort_uniq(Vec *v) {
    if (v->len < 2) return;
    qsort(v->data, (size_t)v->len, sizeof(void *), cmp_str);
    int w = 1;
    for (int i = 1; i < v->len; i++) {
        if (!strcmp((char *)v->data[i], (char *)v->data[w - 1])) free(v->data[i]);
        else v->data[w++] = v->data[i];
    }
    v->len = w;
}

static void add_file_matches(Vec *matches, const char *dir, const char *base, int with_dir) {
    DIR *dp = opendir(dir && *dir ? dir : ".");
    if (!dp) return;
    size_t bl = strlen(base);
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (strlen(de->d_name) < bl) continue;
        if (memcmp(de->d_name, base, bl)) continue;
        if (de->d_name[0] == '.' && base[0] != '.') continue;
        Str full; str_init(&full);
        if (with_dir && dir) str_puts(&full, dir);
        str_puts(&full, de->d_name);
        struct stat st;
        if (stat(full.buf, &st) == 0 && S_ISDIR(st.st_mode)) str_putc(&full, '/');
        else str_putc(&full, ' ');
        vec_push(matches, str_done(&full));
    }
    closedir(dp);
}

/* Complete the word ending at `pos`. Returns 0 if nothing matched, 1 if a
 * unique match was found, 2 if several did (the candidates are listed and the
 * longest common prefix is offered). *out is the text to append at `pos`. */
static int complete_word(const char *buf, int pos, char **out) {
    *out = NULL;
    /* find the word to complete */
    int start = pos;
    while (start > 0 && !strchr(" \t;|&()<>", buf[start - 1])) start--;
    int wlen = pos - start;
    if (wlen == 0) return 0;
    char *prefix = xstrndup(buf + start, wlen);
    int has_slash = strchr(prefix, '/') != NULL;
    int command_pos = 1;
    for (int i = 0; i < start; i++) {
        if (!strchr(" \t", buf[i])) command_pos = strchr(";|&()", buf[i]) != NULL;
    }
    int pad_space = 0;      /* command/variable names get a trailing space */
    Vec matches; vec_init(&matches);

    if (prefix[0] == '$') {
        /* $VAR and ${VAR completion */
        const char *nm = prefix + 1;
        int brace = (nm[0] == '{');
        if (brace) nm++;
        Vec names; vec_init(&names);
        var_names_matching(nm, &names);
        for (int i = 0; i < names.len; i++) {
            Str m; str_init(&m);
            str_putc(&m, '$');
            if (brace) str_putc(&m, '{');
            str_puts(&m, (char *)names.data[i]);
            if (brace) str_putc(&m, '}');
            vec_push(&matches, str_done(&m));
        }
        vec_free(&names);
        pad_space = 1;
    } else if (!has_slash) {
        if (command_pos) {
            /* builtins + functions + PATH lookup */
            for (Builtin *b = builtins; b->name; b++)
                if (strlen(b->name) >= (size_t)wlen && !memcmp(b->name, prefix, wlen))
                    vec_push(&matches, xstrdup(b->name));
            for (size_t i = map_next_used(&g_funs, 0); i < g_funs.cap; i = map_next_used(&g_funs, i + 1)) {
                const char *nm = g_funs.keys[i];
                if (strlen(nm) >= (size_t)wlen && !memcmp(nm, prefix, wlen))
                    vec_push(&matches, xstrdup(nm));
            }
            const char *path = var_get("PATH");
            if (path) {
                Str dir; str_init(&dir);
                for (const char *p = path; ; ) {
                    const char *c = strchr(p, ':');
                    size_t seg = c ? (size_t)(c - p) : strlen(p);
                    str_clear(&dir);
                    str_putn(&dir, p, seg);
                    DIR *dp = opendir(seg ? dir.buf : ".");
                    if (dp) {
                        struct dirent *de;
                        while ((de = readdir(dp))) {
                            if (strlen(de->d_name) < (size_t)wlen) continue;
                            if (memcmp(de->d_name, prefix, wlen)) continue;
                            Str full; str_init(&full);
                            str_puts(&full, seg ? dir.buf : ".");
                            str_putc(&full, '/');
                            str_puts(&full, de->d_name);
                            if (access(full.buf, X_OK) == 0) vec_push(&matches, xstrdup(de->d_name));
                            str_free(&full);
                        }
                        closedir(dp);
                    }
                    if (!c) break;
                    p = c + 1;
                }
                str_free(&dir);
            }
            pad_space = 1;
        }
        add_file_matches(&matches, ".", prefix, 0);
    } else {
        /* file completion with a directory part */
        Str dir; str_init(&dir);
        const char *slash = strrchr(prefix, '/');
        str_putn(&dir, prefix, (size_t)(slash - prefix) + 1);
        const char *base = slash + 1;
        add_file_matches(&matches, dir.buf, base, 1);
        str_free(&dir);
    }
    free(prefix);

    if (matches.len == 0) { vec_free(&matches); return 0; }
    vec_sort_uniq(&matches);

    /* one match: append just the part that was not typed yet */
    if (matches.len == 1) {
        Str s; str_init(&s);
        str_puts(&s, (char *)matches.data[0] + wlen);
        if (pad_space) str_putc(&s, ' ');
        *out = str_done(&s);
        vec_free(&matches);
        return 1;
    }
    /* many matches: offer the longest common prefix, then list them */
    size_t common = strlen((char *)matches.data[0]);
    for (int i = 1; i < matches.len; i++) {
        size_t l = strlen((char *)matches.data[i]);
        if (l < common) common = l;
        for (size_t j = 0; j < common; j++)
            if (((char *)matches.data[i])[j] != ((char *)matches.data[0])[j]) { common = j; break; }
    }
    if (common > (size_t)wlen)
        *out = xstrndup((char *)matches.data[0] + wlen, common - (size_t)wlen);
    fputs("\n", stdout);
    for (int i = 0; i < matches.len; i++)
        printf("%s  ", (char *)matches.data[i]);
    fputs("\n", stdout);
    vec_free(&matches);
    return 2;
}

int edit_complete(const char *buf, int pos, char **out, int *common) {
    if (common) *common = 0;
    return complete_word(buf, pos, out);
}

/* ---------------- prompt construction ---------------- */
char *prompt_string(const char *ps) {
    Str out; str_init(&out);
    /* expand PS1: \u \h \w \W \$ \t \d \n \\ \[ \] */
    for (const char *p = ps ? ps : "$ "; *p; p++) {
        if (*p != '\\') { str_putc(&out, *p); continue; }
        p++;
        switch (*p) {
        case 'u': {
            struct passwd *pw = getpwuid(getuid());
            str_puts(&out, pw ? pw->pw_name : "user");
            break;
        }
        case 'h': case 'H': {
            char h[256] = {0};
            gethostname(h, sizeof h - 1);
            if (*p == 'h') { char *dot = strchr(h, '.'); if (dot) *dot = 0; }
            str_puts(&out, h);
            break;
        }
        case 'w': case 'W': {
            char buf[4096];
            if (!getcwd(buf, sizeof buf)) strcpy(buf, "?");
            const char *hd = home_dir();
            if (hd && !strncmp(buf, hd, strlen(hd))) {
                str_putc(&out, '~');
                str_puts(&out, buf + strlen(hd));
            } else if (*p == 'W') {
                const char *base = strrchr(buf, '/');
                str_puts(&out, base ? base + 1 : buf);
            } else str_puts(&out, buf);
            break;
        }
        case '$': str_putc(&out, geteuid() == 0 ? '#' : '$'); break;
        case 't': {
            time_t now = time(NULL);
            struct tm tmv;
            localtime_r(&now, &tmv);
            char tb[16];
            strftime(tb, sizeof tb, "%H:%M:%S", &tmv);
            str_puts(&out, tb);
            break;
        }
        case 'd': {
            time_t now = time(NULL);
            struct tm tmv;
            localtime_r(&now, &tmv);
            char tb[32];
            strftime(tb, sizeof tb, "%a %b %d", &tmv);
            str_puts(&out, tb);
            break;
        }
        case 'n': str_putc(&out, '\n'); break;
        case 'e': str_putc(&out, '\x1b'); break;
        case 'a': str_putc(&out, '\a'); break;
        case '[': case ']': break;   /* color markers: ignored */
        case '\\': str_putc(&out, '\\'); break;
        case 's': str_puts(&out, OSH_NAME); break;
        case 'v': str_puts(&out, OSH_VERSION); break;
        case 0: str_putc(&out, '\\'); goto done;
        default: str_putc(&out, *p); break;
        }
    }
done:
    /* variable expansion in PS1 */
    {
        Vec v; vec_init(&v);
        expand_str(out.buf, &v, 0);
        if (v.len) {
            Str j; str_init(&j);
            for (int i = 0; i < v.len; i++) {
                if (i) str_putc(&j, ' ');
                str_puts(&j, (char *)v.data[i]);
            }
            str_free(&out);
            out = j;
        }
        vec_free(&v);
    }
    return str_done(&out);
}

/* ---------------- the main edit loop ---------------- */
char *edit_getline(const char *prompt_raw) {
    if (!editor_ok) {
        /* not a terminal: plain line read */
        Str line; str_init(&line);
        int c = getchar();
        if (c == EOF) return NULL;
        while (c != EOF && c != '\n') { str_putc(&line, (char)c); c = getchar(); }
        return str_done(&line);
    }
    char *prompt = prompt_string(prompt_raw);
    Str buf; str_init(&buf);
    str_grow(&buf, 0);
    int pos = 0, len = 0;
    hist_pos = hist_count;
    raw_on();
    fputs(prompt, stdout);
    fflush(stdout);
    for (;;) {
        int c = getchar();
        if (c == EOF) { raw_off(); free(prompt); return NULL; }
        if (c == '\x1b') {
            int c1 = getchar();
            int c2 = getchar();
            if (c1 == '[' || c1 == 'O') {
                switch (c2) {
                case 'A':                     /* up: history back */
                    if (hist_pos > 0) {
                        hist_pos--;
                        len = strlen(hist[hist_pos]);
                        buf.len = 0;
                        str_putn(&buf, hist[hist_pos], len);
                        pos = len;
                        refresh(prompt, buf.buf, pos, len);
                    }
                    continue;
                case 'B':                     /* down: history forward */
                    if (hist_pos < hist_count - 1) {
                        hist_pos++;
                        len = strlen(hist[hist_pos]);
                        buf.len = 0;
                        str_putn(&buf, hist[hist_pos], len);
                        pos = len;
                        refresh(prompt, buf.buf, pos, len);
                    } else if (hist_pos == hist_count - 1) {
                        hist_pos++;
                        len = 0; pos = 0; buf.len = 0;
                        refresh(prompt, buf.buf, pos, len);
                    }
                    continue;
                case 'C':                     /* right */
                    if (pos < len) { pos++; refresh(prompt, buf.buf, pos, len); }
                    continue;
                case 'D':                     /* left */
                    if (pos > 0) { pos--; refresh(prompt, buf.buf, pos, len); }
                    continue;
                case 'H':                     /* home */
                    pos = 0; refresh(prompt, buf.buf, pos, len);
                    continue;
                case 'F':                     /* end */
                    pos = len; refresh(prompt, buf.buf, pos, len);
                    continue;
                case '3':                     /* delete (Del) */
                    getchar();               /* trailing ~ */
                    if (pos < len) {
                        memmove(buf.buf + pos, buf.buf + pos + 1, (size_t)(len - pos));
                        len--; buf.len = len;
                        refresh(prompt, buf.buf, pos, len);
                    }
                    continue;
                default: continue;
                }
            }
            if (c1 == '[' && (c2 >= '1' && c2 <= '9')) { getchar(); continue; }
            continue;
        }
        switch (c) {
        case 4:                               /* Ctrl-D at empty line: EOF */
            if (len == 0) {
                raw_off();
                free(prompt);
                if (g_opt_ignoreeof) { printf("\n"); return xstrdup(""); }
                printf("exit\n");
                return NULL;
            }
            break;
        case 3:                               /* Ctrl-C: discard line */
            fputs("^C\n", stdout);
            raw_off();
            free(prompt);
            return xstrdup("");
        case 21:                              /* Ctrl-U: kill to start */
            memmove(buf.buf, buf.buf + pos, (size_t)(len - pos));
            len -= pos; buf.len = len; pos = 0;
            refresh(prompt, buf.buf, pos, len);
            continue;
        case 11:                              /* Ctrl-K: kill to end */
            len = pos; buf.len = len;
            refresh(prompt, buf.buf, pos, len);
            continue;
        case 1:                               /* Ctrl-A: home */
            pos = 0; refresh(prompt, buf.buf, pos, len);
            continue;
        case 5:                               /* Ctrl-E: end */
            pos = len; refresh(prompt, buf.buf, pos, len);
            continue;
        case 12: {                            /* Ctrl-L: clear screen */
            fputs("\x1b[H\x1b[2J", stdout);
            refresh(prompt, buf.buf, pos, len);
            continue;
        }
        case 23: {                            /* Ctrl-W: delete word back */
            int start = pos;
            while (start > 0 && buf.buf[start - 1] == ' ') start--;
            while (start > 0 && buf.buf[start - 1] != ' ') start--;
            memmove(buf.buf + start, buf.buf + pos, (size_t)(len - pos));
            len -= (pos - start);
            buf.len = (size_t)len;
            pos = start;
            refresh(prompt, buf.buf, pos, len);
            continue;
        }
        case 9: {                             /* Tab: completion */
            char *insert = NULL;
            int r = complete_word(buf.buf, pos, &insert);
            if (insert) {
                size_t il = strlen(insert);
                str_grow(&buf, il);
                memmove(buf.buf + pos + il, buf.buf + pos, (size_t)(len - pos));
                memcpy(buf.buf + pos, insert, il);
                len += (int)il; buf.len = (size_t)len; pos += (int)il;
                free(insert);
            }
            if (r) refresh(prompt, buf.buf, pos, len);
            continue;
        }
        case '\r': case '\n':                 /* submit */
            buf.buf[len] = 0;
            fputs("\n", stdout);
            fflush(stdout);
            raw_off();
            char *result = xstrdup(buf.buf);
            str_free(&buf);
            free(prompt);
            return result;
        case 127: case 8:                     /* backspace */
            if (pos > 0) {
                memmove(buf.buf + pos - 1, buf.buf + pos, (size_t)(len - pos));
                pos--; len--; buf.len = len;
                refresh(prompt, buf.buf, pos, len);
            }
            continue;
        default:
            if (c >= 0 && c < 32) continue;
            str_grow(&buf, 1);
            memmove(buf.buf + pos + 1, buf.buf + pos, (size_t)(len - pos));
            buf.buf[pos] = (char)c;
            pos++; len++; buf.len = (size_t)len;
            refresh(prompt, buf.buf, pos, len);
            continue;
        }
    }
}
