/* util.c - allocation, growable string/vector/map, path helpers */
#include "osh.h"
#include <errno.h>
#include <ctype.h>
#include <pwd.h>
#include <fcntl.h>

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "osh: out of memory\n"); _exit(1); }
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "osh: out of memory\n"); _exit(1); }
    return q;
}

char *xstrdup(const char *s) {
    if (!s) return NULL;
    return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n) {
    if (n > (size_t)-1 - 1) { fprintf(stderr, "osh: out of memory\n"); _exit(1); }
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

int xstreq(const char *a, const char *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    return strcmp(a, b) == 0;
}

/* ---------------- growable string ---------------- */
void str_init(Str *s) { s->buf = NULL; s->len = 0; s->cap = 0; }
void str_free(Str *s) { free(s->buf); str_init(s); }
void str_clear(Str *s) { s->len = 0; if (s->buf) s->buf[0] = 0; }

void str_grow(Str *s, size_t extra) {
    if (extra > (size_t)-1 - 1) { fprintf(stderr, "osh: out of memory\n"); _exit(1); }
    size_t need = extra + 1;
    if (s->len > (size_t)-1 - need) { fprintf(stderr, "osh: out of memory\n"); _exit(1); }
    size_t min = s->len + need;
    if (min <= s->cap) return;
    size_t want = s->cap ? s->cap : 64;
    while (want < min) {
        if (want > (size_t)-1 / 2) { want = min; break; }
        want *= 2;
    }
    s->buf = xrealloc(s->buf, want);
    s->cap = want;
}

void str_putc(Str *s, char c) {
    str_grow(s, 1);
    s->buf[s->len++] = c;
    s->buf[s->len] = 0;
}

void str_putn(Str *s, const char *t, size_t n) {
    if (n == (size_t)-1) return;
    if (!n) return;
    str_grow(s, n);
    memcpy(s->buf + s->len, t, n);
    s->len += n;
    s->buf[s->len] = 0;
}

void str_puts(Str *s, const char *t) {
    if (t) str_putn(s, t, strlen(t));
}

void str_printf(Str *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return; }
    str_grow(s, (size_t)n);
    vsnprintf(s->buf + s->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    s->len += (size_t)n;
}

char *str_done(Str *s) {
    char *p = s->buf ? s->buf : xstrdup("");
    str_init(s);
    return p;
}

/* ---------------- vector ---------------- */
void vec_init(Vec *v) { v->data = NULL; v->len = 0; v->cap = 0; }

void vec_push(Vec *v, void *p) {
    if (v->len >= v->cap) {
        if (v->cap > 0x3fffffff) { fprintf(stderr, "osh: out of memory\n"); _exit(1); }
        v->cap = v->cap ? v->cap * 2 : 8;
        v->data = xrealloc(v->data, (size_t)v->cap * sizeof(void *));
    }
    v->data[v->len++] = p;
}

void vec_free(Vec *v) {   /* frees the void* pointers themselves */
    for (int i = 0; i < v->len; i++) free(v->data[i]);
    free(v->data);
    vec_init(v);
}

/* ---------------- hash map ---------------- */
static unsigned long map_hash(const char *s) {
    unsigned long h = 1469598103934665603UL;   /* FNV-1a 64 */
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211UL;
    }
    return h;
}

static void map_rehash(Map *m) {
    size_t ncap = m->cap ? m->cap * 2 : 32;
    char **nk = xmalloc(ncap * sizeof(char *));
    char **nv = xmalloc(ncap * sizeof(char *));
    unsigned char *nu = xmalloc(ncap);
    memset(nk, 0, ncap * sizeof(char *));
    memset(nv, 0, ncap * sizeof(char *));
    memset(nu, 0, ncap);
    for (size_t i = 0; i < m->cap; i++) {
        if (!m->used[i]) continue;
        size_t j = map_hash(m->keys[i]) & (ncap - 1);
        while (nu[j]) j = (j + 1) & (ncap - 1);
        nk[j] = m->keys[i];
        nv[j] = m->vals[i];
        nu[j] = 1;
    }
    free(m->keys); free(m->vals); free(m->used);
    m->keys = nk; m->vals = nv; m->used = nu; m->cap = ncap;
}

void map_init(Map *m) {
    m->keys = NULL; m->vals = NULL; m->used = NULL;
    m->cap = 0; m->size = 0;
}

void map_free(Map *m) {
    for (size_t i = 0; i < m->cap; i++)
        if (m->used[i]) { free(m->keys[i]); free(m->vals[i]); }
    free(m->keys); free(m->vals); free(m->used);
    map_init(m);
}

void map_put(Map *m, const char *k, const char *v) {
    if (m->cap == 0) map_rehash(m);
    else if (m->size * 4 >= m->cap * 3) map_rehash(m);
    size_t j = map_hash(k) & (m->cap - 1);
    while (m->used[j]) {
        if (strcmp(m->keys[j], k) == 0) {
            free(m->vals[j]);
            m->vals[j] = xstrdup(v ? v : "");
            return;
        }
        j = (j + 1) & (m->cap - 1);
    }
    m->keys[j] = xstrdup(k);
    m->vals[j] = xstrdup(v ? v : "");
    m->used[j] = 1;
    m->size++;
}

const char *map_get(Map *m, const char *k) {
    if (!m->cap) return NULL;
    size_t j = map_hash(k) & (m->cap - 1);
    while (m->used[j]) {
        if (strcmp(m->keys[j], k) == 0) return m->vals[j];
        j = (j + 1) & (m->cap - 1);
    }
    return NULL;
}

int map_del(Map *m, const char *k) {
    if (!m->cap) return 0;
    size_t j = map_hash(k) & (m->cap - 1);
    while (m->used[j]) {
        if (strcmp(m->keys[j], k) == 0) {
            free(m->keys[j]); free(m->vals[j]);
            m->keys[j] = NULL; m->vals[j] = NULL;
            m->used[j] = 0;
            m->size--;
            /* backward-shift for open addressing; move pointers, do not free */
            size_t next = (j + 1) & (m->cap - 1);
            while (m->used[next]) {
                size_t want = map_hash(m->keys[next]) & (m->cap - 1);
                size_t dist = (next - want) & (m->cap - 1);
                size_t dj   = (next - j) & (m->cap - 1);
                if (dist >= dj) {
                    m->keys[j] = m->keys[next]; m->vals[j] = m->vals[next];
                    m->used[j] = 1; m->used[next] = 0;
                    m->keys[next] = NULL; m->vals[next] = NULL;
                    j = next;
                } else break;
                next = (next + 1) & (m->cap - 1);
            }
            return 1;
        }
        j = (j + 1) & (m->cap - 1);
    }
    return 0;
}

size_t map_next_used(Map *m, size_t i) {
    for (; i < m->cap; i++) if (m->used[i]) return i;
    return m->cap;
}

/* ---------------- path helpers ---------------- */
const char *home_dir(void) {
    const char *h = getenv("HOME");
    if (h && *h) return h;
    struct passwd *pw = getpwuid(getuid());
    if (pw) return pw->pw_dir;
    return "/";
}

char *tilde_expand(const char *s) {
    /* handles "~" and "~user"; returns NULL if ~user is unknown */
    if (!s || s[0] != '~') return NULL;
    if (s[1] == 0 || s[1] == '/') {
        size_t hl = strlen(home_dir());
        char *out = xmalloc(hl + strlen(s + 1) + 1);
        memcpy(out, home_dir(), hl);
        strcpy(out + hl, s + 1);
        return out;
    }
    const char *slash = strchr(s, '/');
    size_t ulen = slash ? (size_t)(slash - s) - 1 : strlen(s + 1);
    char *user = xstrndup(s + 1, ulen);
    struct passwd *pw = getpwnam(user);
    free(user);
    if (!pw) return NULL;
    const char *rest = slash ? slash : "";
    size_t pl = strlen(pw->pw_dir);
    char *out = xmalloc(pl + strlen(rest) + 1);
    memcpy(out, pw->pw_dir, pl);
    strcpy(out + pl, rest);
    return out;
}

int is_directory(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Open a per-user conf/history file. write=0 read, write=1 create/trunc 0600.
 * Rejects non-regular files and files not owned by the current user.
 * Writes do not follow symlinks (O_NOFOLLOW). */
int open_user_file(const char *path, int write) {
    int flags = write ? (O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW) : O_RDONLY;
    flags |= O_CLOEXEC;
    int fd = open(path, flags, 0600);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid()) {
        close(fd);
        return -1;
    }
    if (write) fchmod(fd, 0600);
    return fd;
}
