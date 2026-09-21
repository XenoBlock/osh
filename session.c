/* session.c - shared ("takeover") shell sessions over a UNIX socket.
 *
 * `osh --session ID` attaches to the session named ID. The first attach forks a
 * session server that owns the shell state (variables, cwd, functions) and runs
 * the commands; clients are byte relays of their own terminal. Only one client
 * is active at a time. When another client attaches it takes the session over
 * and the displaced client is told who took it and how to reconnect.
 *
 * Wire format, server -> client: raw command output, plus control lines that
 * start with 0x01 and are only ever sent between commands:
 *     \x01OK <id>\n        attach accepted; the client may now send input
 *     \x01TAKEN <who>\n    displaced by <who>; the server closes the connection
 *
 * ponytail: the transport is a plain byte stream, so a remote session gets the
 * terminal driver's line editing instead of osh's raw-mode editor, and Ctrl-C
 * does not reach a running command (use `kill`). Upgrade path: give each client
 * a pty pair and forward the interrupt byte between commands.
 */
#include "osh.h"
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

#define SESS_MAX_ID 64

static char *sess_dir(void) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    Str s; str_init(&s);
    str_puts(&s, (rt && *rt) ? rt : "/tmp");
    str_printf(&s, "/osh-%u", (unsigned)getuid());
    return str_done(&s);
}

/* Session ids become file names: allow only a conservative character set. */
int session_id_ok(const char *id) {
    if (!id || !*id || strlen(id) > SESS_MAX_ID) return 0;
    if (!strcmp(id, ".") || !strcmp(id, "..")) return 0;
    for (const char *p = id; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr("._-", *p)) return 0;
    return 1;
}

static char *sess_sock_path(const char *id) {
    char *d = sess_dir();
    Str s; str_init(&s);
    str_printf(&s, "%s/%s.sock", d, id);
    free(d);
    return str_done(&s);
}

static int write_all(int fd, const char *buf, size_t n);
static char *sess_pid_path(const char *id);
static int  sess_pid_of(const char *id, pid_t *out);
static void sess_remove_files(const char *id);
static void sess_sigterm(int sig);

int session_list(void) {
    char *dir = sess_dir();
    DIR *dp = opendir(dir);
    if (!dp) {
        free(dir);
        return 0;
    }
    struct dirent *de;
    while ((de = readdir(dp))) {
        size_t n = strlen(de->d_name);
        if (n <= 5 || strcmp(de->d_name + n - 5, ".sock")) continue;
        char *id = xstrndup(de->d_name, n - 5);
        if (session_id_ok(id)) {
            pid_t p;
            if (sess_pid_of(id, &p) && kill(p, 0) == 0)
                printf("%-16s pid %d\n", id, (int)p);
            else
                printf("%-16s not running\n", id);
        }
        free(id);
    }
    closedir(dp);
    free(dir);
    return 0;
}

static void sess_whoami(Str *out) {
    const char *u = getenv("USER");
    if (!u || !*u) {
        struct passwd *pw = getpwuid(getuid());
        u = (pw && pw->pw_name) ? pw->pw_name : "?";
    }
    char host[128] = "?";
    if (gethostname(host, sizeof host - 1) != 0) strcpy(host, "?");
    host[sizeof host - 1] = 0;
    str_printf(out, "%s@%s", u, host);
}

static int write_all(int fd, const char *buf, size_t n) {
    while (n) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        buf += w; n -= (size_t)w;
    }
    return 0;
}

/* The server records its pid next to the socket so that a stale session
 * (killed with SIGKILL, or crashed) can be told apart from a live one. */
static char *sess_pid_path(const char *id) {
    char *d = sess_dir();
    Str s; str_init(&s);
    str_printf(&s, "%s/%s.pid", d, id);
    free(d);
    return str_done(&s);
}

static int sess_pid_of(const char *id, pid_t *out) {
    char *path = sess_pid_path(id);
    int fd = open_user_file(path, 0);
    free(path);
    if (fd < 0) return 0;
    char pb[32];
    ssize_t n = read(fd, pb, sizeof pb - 1);
    close(fd);
    if (n <= 0) return 0;
    pb[n] = 0;
    char *end;
    long p = strtol(pb, &end, 10);
    if (end == pb || p <= 0) return 0;
    *out = (pid_t)p;
    return 1;
}

static void sess_remove_files(const char *id) {
    char *sock = sess_sock_path(id);
    char *pidf = sess_pid_path(id);
    unlink(sock);
    unlink(pidf);
    free(sock);
    free(pidf);
}

static void sess_sigterm(int sig) { (void)sig; exit(0); }

/* Terminate a session server, or clean up its leftovers if it is already gone. */
int session_kill(const char *id) {
    if (!session_id_ok(id)) {
        fprintf(stderr, "osh: session: invalid session id '%s'\n", id ? id : "");
        return 2;
    }
    pid_t p;
    if (!sess_pid_of(id, &p)) {
        fprintf(stderr, "osh: session: '%s': no pid record\n", id ? id : "");
        return 2;
    }
    if (kill(p, 0) != 0) {
        sess_remove_files(id);
        printf("osh: session: '%s' was not running; cleaned up\n", id);
        return 0;
    }
    if (kill(p, SIGTERM) != 0) {
        fprintf(stderr, "osh: session: kill %d: %s\n", (int)p, strerror(errno));
        return 2;
    }
    for (int i = 0; i < 100; i++) {
        if (kill(p, 0) != 0) break;
        usleep(10000);
    }
    sess_remove_files(id);
    printf("osh: session: '%s' terminated\n", id);
    return 0;
}

static int sess_connect(const char *path) {
    struct sockaddr_un sa;
    if (strlen(path) >= sizeof sa.sun_path) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    strcpy(sa.sun_path, path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }
    return fd;
}

/* Only the owning user may attach to a session. */
static int sess_peer_is_me(int fd) {
    struct ucred cr;
    socklen_t len = sizeof cr;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0) return 0;
    return cr.uid == getuid();
}

static void sess_send_prompt(int fd) {
    const char *ps1 = var_get("PS1");
    char *p = prompt_string(ps1 ? ps1 : "$ ");
    write_all(fd, p, strlen(p));
    free(p);
}

static char *g_sess_path = NULL;   /* server: socket to unlink on exit */
static char *g_pid_path = NULL;

static void sess_unlink(void) {
    if (g_sess_path) { unlink(g_sess_path); free(g_sess_path); g_sess_path = NULL; }
    if (g_pid_path)  { unlink(g_pid_path);  free(g_pid_path);  g_pid_path = NULL; }
}

/* Owns the shell state; runs until `exit` is executed in the session. */
static int session_server(const char *id) {
    char *dir = sess_dir();
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "osh: session: %s: %s\n", dir, strerror(errno));
        free(dir);
        return 2;
    }
    struct stat st;
    if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) != 0) {
        fprintf(stderr, "osh: session: %s: refusing unsafe directory\n", dir);
        free(dir);
        return 2;
    }
    free(dir);

    g_sess_path = sess_sock_path(id);
    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0) return 2;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", g_sess_path);
    unlink(g_sess_path);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        fprintf(stderr, "osh: session: bind %s: %s\n", g_sess_path, strerror(errno));
        return 2;
    }
    chmod(g_sess_path, 0600);
    if (listen(lfd, 8) != 0) return 2;
    g_pid_path = sess_pid_path(id);
    int pfd = open_user_file(g_pid_path, 1);
    if (pfd >= 0) {
        char pb[32];
        int pl = snprintf(pb, sizeof pb, "%d\n", (int)getpid());
        write_all(pfd, pb, (size_t)pl);
        close(pfd);
    }
    /* SIGTERM should leave through exit() so atexit can unlink the files. */
    signal(SIGTERM, sess_sigterm);
    atexit(sess_unlink);

    /* No controlling terminal: never die from a client's stray signal. */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    g_interactive = 0;

    int active = -1;
    var_set("OSH_SESSION", id);
    Str pend; str_init(&pend);

    for (;;) {
        struct pollfd pf[2];
        pf[0].fd = lfd;    pf[0].events = POLLIN; pf[0].revents = 0;
        pf[1].fd = active; pf[1].events = POLLIN; pf[1].revents = 0;
        int nf = (active >= 0) ? 2 : 1;
        if (poll(pf, nf, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pf[0].revents & POLLIN) {
            int c = accept(lfd, NULL, NULL);
            if (c >= 0) {
                if (!sess_peer_is_me(c)) { close(c); continue; }
                /* hello line: the attaching user's identity */
                char hb[160];
                ssize_t r = read(c, hb, sizeof hb - 1);
                if (r <= 0) { close(c); continue; }
                hb[r] = 0;
                char *nl = strchr(hb, '\n');
                if (nl) *nl = 0;
                const char *who = *hb ? hb : "someone";
                if (active >= 0) {
                    char m[224];
                    int ml = snprintf(m, sizeof m, "\x01TAKEN %s\n", who);
                    write_all(active, m, (size_t)ml);
                    close(active);
                }
                active = c;
                str_clear(&pend);
                char ok[128];
                int ol = snprintf(ok, sizeof ok, "\x01OK %s\n", id);
                write_all(active, ok, (size_t)ol);
                /* command output goes to whoever is attached */
                dup2(active, 0); dup2(active, 1); dup2(active, 2);
                sess_send_prompt(active);
            }
        }
        if (active >= 0 && (pf[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[4096];
            ssize_t r = read(active, buf, sizeof buf);
            if (r <= 0) { close(active); active = -1; continue; }
            str_putn(&pend, buf, (size_t)r);
            for (;;) {
                char *nl = pend.len ? memchr(pend.buf, '\n', pend.len) : NULL;
                if (!nl) break;
                size_t ll = (size_t)(nl - pend.buf);
                char *line = xstrndup(pend.buf, ll);
                memmove(pend.buf, nl + 1, pend.len - ll - 1);
                pend.len -= ll + 1;
                pend.buf[pend.len] = 0;
                if (*line) {
                    run_string(line);
                    fflush(NULL);
                }
                free(line);
                sess_send_prompt(active);
            }
        }
    }
    return 0;
}

/* Fork a detached server that inherits the shell state we just built. */
static int sess_start_server(const char *id) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setsid();
        _exit(session_server(id));
    }
    return 0;
}

int session_client(const char *id) {
    if (!session_id_ok(id)) {
        fprintf(stderr, "osh: session: invalid session id '%s'\n", id ? id : "");
        return 2;
    }
    char *path = sess_sock_path(id);
    int fd = sess_connect(path);
    if (fd < 0) {
        if (sess_start_server(id) != 0) {
            fprintf(stderr, "osh: session: cannot start a session server\n");
            free(path);
            return 2;
        }
        for (int i = 0; i < 100 && fd < 0; i++) {
            usleep(20000);
            fd = sess_connect(path);
        }
    }
    free(path);
    if (fd < 0) {
        fprintf(stderr, "osh: session: cannot connect to session '%s'\n", id);
        return 2;
    }

    Str who; str_init(&who);
    sess_whoami(&who);
    char hello[192];
    int hl = snprintf(hello, sizeof hello, "%s\n", who.buf);
    str_free(&who);
    if (write_all(fd, hello, (size_t)hl) != 0) { close(fd); return 2; }

    /* Ctrl-C cannot reach the remote command, so don't let it drop the client. */
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    char pend[8192];
    size_t plen = 0;
    int ready = 0, taken = 0, saw_ctrl_s = 0;
    char taker[128] = "";

    for (;;) {
        struct pollfd pf[2];
        pf[0].fd = fd; pf[0].events = POLLIN; pf[0].revents = 0;
        pf[1].fd = 0;  pf[1].events = POLLIN; pf[1].revents = 0;
        int nf = ready ? 2 : 1;
        if (poll(pf, nf, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buf[4096];
            ssize_t r = read(fd, buf, sizeof buf);
            if (r <= 0) break;
            if (plen + (size_t)r > sizeof pend) {   /* never expected: resync */
                write_all(1, pend, plen);
                plen = 0;
            }
            if ((size_t)r > sizeof pend) r = (ssize_t)sizeof pend;
            memcpy(pend + plen, buf, (size_t)r);
            plen += (size_t)r;
            for (;;) {
                char *ctl = memchr(pend, '\x01', plen);
                if (!ctl) {
                    if (plen) { write_all(1, pend, plen); plen = 0; }
                    break;
                }
                size_t off = (size_t)(ctl - pend);
                if (off) {
                    write_all(1, pend, off);
                    memmove(pend, pend + off, plen - off);
                    plen -= off;
                }
                char *nl = memchr(pend, '\n', plen);
                if (!nl) break;                     /* frame not complete yet */
                size_t flen = (size_t)(nl - pend);
                if (flen >= 4 && !memcmp(pend, "\x01OK ", 4)) ready = 1;
                else if (flen >= 7 && !memcmp(pend, "\x01TAKEN ", 7)) {
                    snprintf(taker, sizeof taker, "%.*s", (int)(flen - 7), pend + 7);
                    taken = 1;
                }
                memmove(pend, nl + 1, plen - flen - 1);
                plen -= flen + 1;
            }
            if (taken) break;
        }
        if (ready && (pf[1].revents & POLLIN)) {
            char buf[1024];
            ssize_t r = read(0, buf, sizeof buf);
            if (r <= 0) break;                      /* local stdin closed */
            Str out; str_init(&out);
            for (ssize_t i = 0; i < r; i++) {
                unsigned char c = (unsigned char)buf[i];
                if (saw_ctrl_s) {
                    if (c == 4) {                    /* Ctrl-S then Ctrl-D: detach */
                        str_free(&out);
                        close(fd);
                        printf("\nDetached from session %s\n", id);
                        return 0;
                    }
                    str_putc(&out, 19);
                    saw_ctrl_s = 0;
                }
                if (c == 19) saw_ctrl_s = 1;         /* Ctrl-S */
                else str_putc(&out, (char)c);
            }
            if (out.len && write_all(fd, out.buf, out.len) != 0) { str_free(&out); break; }
            str_free(&out);
        }
    }
    close(fd);
    if (taken)
        printf("\nThe session has been taken by %s, type osh --session %s to reconnect\n",
               taker, id);
    return 0;
}
