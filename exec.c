/* exec.c - command execution: redirections, pipelines, job control,
 *          conditionals, loops and function calls. */
#include "osh.h"
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <pwd.h>

int   g_status = 0;
pid_t g_last_bg = 0;
int   g_interactive = 0;
jmp_buf g_exit_jmp;
int   g_exit_jmp_set = 0;
int   g_flow = FLOW_NONE;
int   g_flow_level = 0;

/* ---------- job table ---------- */
/* the job table lives in jobs.c; declared in osh.h */

static void job_set_cmd(int slot, Node *n) {
    Str s; str_init(&s);
    node_to_str(n, &s);
    free(jobs[slot].cmd);
    jobs[slot].cmd = str_done(&s);
}

int job_add(pid_t pid, Node *n) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobs[i].pid == 0) {
            jobs[i].pid = pid;
            jobs[i].job = next_job++;
            jobs[i].running = 1;
            jobs[i].reported = 0;
            job_set_cmd(i, n);
            return i;
        }
    }
    return -1;
}

void jobs_reap(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].pid) continue;
        int st;
        pid_t r = waitpid(jobs[i].pid, &st, WNOHANG | WUNTRACED);
        if (r == 0) continue;
        if (r < 0) { jobs[i].pid = 0; continue; }
        if (WIFSTOPPED(st)) { jobs[i].running = 0; jobs[i].reported = 0; }
        else { jobs[i].running = -1; jobs[i].reported = 0; }
    }
}

void jobs_print_changes(void) {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].pid || jobs[i].reported) continue;
        if (jobs[i].running == -1) {
            printf("[%d] Done\t%s\n", jobs[i].job, jobs[i].cmd ? jobs[i].cmd : "");
            jobs[i].pid = 0;
            jobs[i].reported = 1;
        } else if (jobs[i].running == 0) {
            printf("[%d] Stopped\t%s\n", jobs[i].job, jobs[i].cmd ? jobs[i].cmd : "");
            jobs[i].reported = 1;
        }
    }
}

/* ---------- signal handling ---------- */
static volatile sig_atomic_t got_sigint = 0;
static volatile sig_atomic_t got_sigtstp = 0;

static void on_sigint(int sig) { (void)sig; got_sigint = 1; }
static void on_sigtstp(int sig) { (void)sig; got_sigtstp = 1; }
static void on_sigchld(int sig) { (void)sig; }

void sigchld_arm(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
}

void sigchld_block(void)   { sigset_t s; sigemptyset(&s); sigaddset(&s, SIGCHLD); sigprocmask(SIG_BLOCK, &s, NULL); }
void sigchld_unblock(void) { sigset_t s; sigemptyset(&s); sigaddset(&s, SIGCHLD); sigprocmask(SIG_UNBLOCK, &s, NULL); }

void shell_signals_init(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    if (g_interactive) {
        sa.sa_handler = on_sigint;
        sigaction(SIGINT, &sa, NULL);
        sa.sa_handler = on_sigtstp;
        sigaction(SIGTSTP, &sa, NULL);
        sigaction(SIGTTIN, &sa, NULL);
        sigaction(SIGTTOU, &sa, NULL);
    } else {
        sa.sa_handler = SIG_DFL;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTSTP, &sa, NULL);
    }
    sigchld_arm();
}

/* ---------- trap support (minimal: EXIT only, plus INT/ERR hooks) ---------- */
Map g_traps = {0};
void trap_set(const char *name, const char *body) { map_put(&g_traps, name, body); }
const char *trap_get(const char *name) { return map_get(&g_traps, name); }
void trap_run(int sig) {
    const char *names[] = { "EXIT", "INT", "ERR", "TERM", "HUP", NULL };
    const char *body = NULL;
    if (sig == 0) body = map_get(&g_traps, "EXIT");
    else if (sig >= 1 && sig <= 4) body = map_get(&g_traps, names[sig]);
    if (!body || !*body) return;
    int saved_flow = g_flow;
    g_flow = FLOW_NONE;
    run_string(body);
    g_flow = saved_flow;
}

/* ---------- redirection application ---------- */
typedef struct { int fd; int saved; } SavedFd;
static SavedFd saved_fds[32];
static int n_saved = 0;

static int save_fd(int fd) {
    if (n_saved >= 32) return -1;
    int saved = fcntl(fd, F_DUPFD, 10);
    if (saved < 0) return -1;
    saved_fds[n_saved].fd = fd;
    saved_fds[n_saved].saved = saved;
    n_saved++;
    return 0;
}

static int apply_redir(Redir *r) {
    int target_fd = -1;
    char *expanded_target = NULL;
    if (r->type == R_HEREDOC || r->type == R_DHEREDOC || r->type == R_HERESTR) {
        char *body = r->heredoc;
        if (r->type == R_HERESTR) {
            Vec v; vec_init(&v);
            expand_word(&r->target, &v, 0);
            Str j; str_init(&j);
            for (int i = 0; i < v.len; i++) {
                if (i) str_putc(&j, ' ');
                str_puts(&j, (char *)v.data[i]);
            }
            body = str_done(&j);
            vec_free(&v);
        }
        int pfd[2];
        if (pipe(pfd) != 0) return -1;
        size_t bl = body ? strlen(body) : 0;
        if (bl) write(pfd[1], body, bl);
        if (bl && body[bl-1] != '\n') write(pfd[1], "\n", 1);
        close(pfd[1]);
        if (r->type == R_HERESTR) free(body);
        target_fd = pfd[0];
        save_fd(r->fd);
        dup2(target_fd, r->fd);
        close(target_fd);
        return 0;
    }
    /* normal file redirection: expand the target word */
    {
        Vec v; vec_init(&v);
        expand_word(&r->target, &v, EX_SPLIT | EX_GLOB);
        if (v.len != 1) {
            fprintf(stderr, "osh: %s: ambiguous redirect\n", r->target.s ? "" : "");
            vec_free(&v);
            return -1;
        }
        expanded_target = xstrdup((char *)v.data[0]);
        vec_free(&v);
    }
    const char *path = expanded_target;
    if (!path) return -1;
    int fd = r->fd;
    save_fd(fd);
    switch (r->type) {
    case R_IN: {
        int f = open(path, O_RDONLY);
        if (f < 0) { fprintf(stderr, "osh: %s: %s\n", path, strerror(errno)); return -1; }
        dup2(f, fd); close(f);
        break;
    }
    case R_OUT:
    case R_CLOBBER: {
        int f = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (f < 0) { fprintf(stderr, "osh: %s: %s\n", path, strerror(errno)); return -1; }
        dup2(f, fd); close(f);
        break;
    }
    case R_APP: {
        int f = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (f < 0) { fprintf(stderr, "osh: %s: %s\n", path, strerror(errno)); return -1; }
        dup2(f, fd); close(f);
        break;
    }
    case R_ERR: {
        int f = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (f < 0) { fprintf(stderr, "osh: %s: %s\n", path, strerror(errno)); return -1; }
        dup2(f, 2); close(f);
        break;
    }
    case R_ERRAPP: {
        int f = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (f < 0) { fprintf(stderr, "osh: %s: %s\n", path, strerror(errno)); return -1; }
        dup2(f, 2); close(f);
        break;
    }
    case R_DUPOUT: {
        int f = atoi(path);
        if (f <= 0) f = 1;
        dup2(f, fd);
        break;
    }
    case R_DUPIN: {
        int f = atoi(path);
        if (f <= 0) f = 0;
        dup2(f, fd);
        break;
    }
    case R_INOUT: {
        int f = open(path, O_RDWR | O_CREAT, 0644);
        if (f < 0) { fprintf(stderr, "osh: %s: %s\n", path, strerror(errno)); return -1; }
        dup2(f, fd); close(f);
        break;
    }
    default: break;
    }
    free(expanded_target);
    return 0;
}

static int apply_redirs(Node *n) {
    for (int i = 0; i < n->nredirs; i++)
        if (apply_redir(&n->redirs[i]) < 0) return -1;
    return 0;
}

static void restore_redirs(void) {
    while (n_saved > 0) {
        n_saved--;
        dup2(saved_fds[n_saved].saved, saved_fds[n_saved].fd);
        close(saved_fds[n_saved].saved);
    }
}

/* ---------- command resolution ---------- */
char *command_resolve(const char *name) {
    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0) return xstrdup(name);
        return NULL;
    }
    /* function */
    if (map_get(&g_funs, name)) return xstrdup(name);
    /* builtin */
    for (Builtin *b = builtins; b->name; b++)
        if (!strcmp(b->name, name)) return xstrdup(name);
    const char *path = var_get("PATH");
    if (!path) path = "/bin:/usr/bin";
    Str cur; str_init(&cur);
    const char *p = path;
    while (*p) {
        const char *c = strchr(p, ':');
        size_t seg = c ? (size_t)(c - p) : strlen(p);
        str_putn(&cur, p, seg);
        if (!seg) str_puts(&cur, ".");
        str_putc(&cur, '/');
        str_puts(&cur, name);
        if (access(cur.buf, X_OK) == 0) {
            char *r = str_done(&cur);
            return r;
        }
        str_clear(&cur);
        if (!c) break;
        p = c + 1;
    }
    str_free(&cur);
    if (g_opt_autoopen) {
        struct stat st;
        if (stat(name, &st) == 0 && !S_ISDIR(st.st_mode)) {
            char pathbuf[PATH_MAX];
            snprintf(pathbuf, sizeof(pathbuf), "./%s", name);
            return xstrdup(pathbuf);
        }
    }
    return NULL;
}

int is_builtin(const char *name) {
    for (Builtin *b = builtins; b->name; b++)
        if (!strcmp(b->name, name)) return 1;
    return 0;
}

/* build argv from a command node */
static char **build_argv(Node *n, int *argc_out) {
    Vec v; vec_init(&v);
    for (int i = 0; i < n->nargs; i++) {
        expand_word(&n->args[i], &v, EX_SPLIT | EX_GLOB);
    }
    char **argv = xmalloc((v.len + 1) * sizeof(char *));
    for (int i = 0; i < v.len; i++) argv[i] = (char *)v.data[i];
    argv[v.len] = 0;
    *argc_out = (int)v.len;
    free(v.data);
    return argv;
}

static void free_argv(char **argv) {
    if (!argv) return;
    for (char **a = argv; *a; a++) free(*a);
    free(argv);
}

static void apply_assigns(Node *n, int export_them) {
    for (int i = 0; i < n->nassigns; i++) {
        char *raw = n->assigns[i];
        char *eq = strchr(raw, '=');
        if (!eq) continue;
        Vec v; vec_init(&v);
        expand_str(eq + 1, &v, 0);
        Str j; str_init(&j);
        for (int k = 0; k < v.len; k++) {
            if (k) str_putc(&j, ' ');
            str_puts(&j, (char *)v.data[k]);
        }
        char *val = str_done(&j);
        var_setl(raw, eq - raw, val);
        if (export_them) var_mark_exportl(raw, eq - raw);
        free(val);
        vec_free(&v);
    }
}

static void trace_command(char **argv) {
    if (!g_opt_xtrace) return;
    fprintf(stderr, "+");
    for (char **a = argv; *a; a++) fprintf(stderr, " %s", *a);
    fputc('\n', stderr);
}

/* run a function body (stored as source text) with positional params */
static int run_function(const char *name, int argc, char **argv) {
    const char *body = map_get(&g_funs, name);
    if (!body) return 127;
    char **old_pos = g_posargs;
    int old_n = g_nposargs;
    char **pos = xmalloc(argc * sizeof(char *));
    for (int i = 1; i < argc; i++) pos[i - 1] = xstrdup(argv[i]);
    g_posargs = pos;
    g_nposargs = argc - 1;
    scope_push();
    run_string(body);
    scope_pop();
    for (int i = 0; i < g_nposargs; i++) free(g_posargs[i]);
    free(g_posargs);
    g_posargs = old_pos;
    g_nposargs = old_n;
    return g_status;
}

/* ---------- simple command ---------- */
/* execute an external command with optional auto-open fallback */
static void exec_external_cmd(char **argv) {
    execvp(argv[0], argv);
    if (errno == ENOENT && g_opt_autoopen && !strchr(argv[0], '/')) {
        struct stat st;
        if (stat(argv[0], &st) == 0 && !S_ISDIR(st.st_mode)) {
            /* Try running as a script directly if it has execute permission */
            if (access(argv[0], X_OK) == 0) {
                char pathbuf[PATH_MAX];
                snprintf(pathbuf, sizeof(pathbuf), "./%s", argv[0]);
                execvp(pathbuf, argv);
            }
            /* Otherwise, open it with system/user opener (XDG_OPEN / PAGER / EDITOR / xdg-open) */
            const char *opener = var_get("OSH_OPENER");
            if (!opener || !*opener) opener = var_get("OPENER");
            if (!opener || !*opener) {
                if (access("/usr/bin/xdg-open", X_OK) == 0) opener = "xdg-open";
                else if (access("/bin/xdg-open", X_OK) == 0) opener = "xdg-open";
                else opener = var_get("PAGER");
            }
            if (!opener || !*opener) opener = var_get("EDITOR");
            if (!opener || !*opener) opener = "cat";

            /* Count argv */
            int c = 0;
            while (argv[c]) c++;
            char **new_argv = xmalloc(sizeof(char *) * (c + 2));
            new_argv[0] = (char *)opener;
            for (int j = 0; j < c; j++) new_argv[j + 1] = argv[j];
            new_argv[c + 1] = NULL;
            execvp(opener, new_argv);
            /* fallback to cat if opener fails */
            if (strcmp(opener, "cat") != 0) {
                new_argv[0] = "cat";
                execvp("cat", new_argv);
            }
            free(new_argv);
        }
    }
    fprintf(stderr, "osh: %s: %s\n", argv[0], errno == ENOENT ? "command not found" : strerror(errno));
    _exit(errno == ENOENT ? 127 : 126);
}

int exec_simple(Node *n) {
    /* assignment-only command */
    if (n->nargs == 0) {
        apply_assigns(n, 1);
        return 0;
    }
    /* expand, then check for alias/function/builtin/external */
    Vec v; vec_init(&v);
    for (int i = 0; i < n->nargs; i++)
        expand_word(&n->args[i], &v, EX_SPLIT | EX_GLOB);
    if (v.len == 0) {
        apply_assigns(n, 1);
        vec_free(&v);
        return 0;
    }
    /* turn the vector into argv */
    int argc = (int)v.len;
    char **argv = xmalloc((argc + 1) * sizeof(char *));
    for (int i = 0; i < argc; i++) argv[i] = (char *)v.data[i];
    argv[argc] = 0;
    free(v.data);

    /* prefix assignments apply to the command's environment */
    if (n->nassigns && !is_builtin(argv[0]) && !map_get(&g_funs, argv[0])) {
        /* handled in the fork below */
    }

    const char *cmd = argv[0];
    int was_builtin = 0;

    /* function? */
    if (map_get(&g_funs, cmd)) {
        int saved_stdin = dup(0), saved_stdout = dup(1), saved_stderr = dup(2);
        n_saved = 0;
        if (apply_redirs(n) == 0) {
            apply_assigns(n, 0);
            g_status = run_function(cmd, argc, argv);
            fflush(NULL);
        } else g_status = 1;
        restore_redirs();
        dup2(saved_stdin, 0); dup2(saved_stdout, 1); dup2(saved_stderr, 2);
        close(saved_stdin); close(saved_stdout); close(saved_stderr);
        free_argv(argv);
        return g_status;
    }
    /* alias? re-parse the replacement text with the remaining args */
    {
        const char *body = map_get(&g_aliases, cmd);
        if (body) {
            Str src; str_init(&src);
            str_puts(&src, body);
            for (int i = 1; i < argc; i++) { str_putc(&src, ' '); str_puts(&src, argv[i]); }
            Reader r; reader_init_buf(&r, src.buf);
            Lexer alx; lex_init(&alx, &r);
            Node *an = parse_line(&alx);
            int rc = 0;
            if (an) { rc = exec_node(an, 0); node_free(an); }
            str_free(&src);
            free_argv(argv);
            return rc;
        }
    }
    /* builtin? */
    if (is_builtin(cmd)) {
        int saved_stdin = dup(0), saved_stdout = dup(1), saved_stderr = dup(2);
        n_saved = 0;
        if (apply_redirs(n) == 0) {
            apply_assigns(n, 0);
            trace_command(argv);
            g_status = run_builtin(argc, argv);
            fflush(NULL);                /* drain stdio before restoring fds */
            was_builtin = 1;
        } else g_status = 1;
        restore_redirs();
        dup2(saved_stdin, 0); dup2(saved_stdout, 1); dup2(saved_stderr, 2);
        close(saved_stdin); close(saved_stdout); close(saved_stderr);
        free_argv(argv);
        (void)was_builtin;
        return g_status;
    }
    /* external: fork */
    pid_t pid = fork();
    if (pid < 0) { fprintf(stderr, "osh: fork: %s\n", strerror(errno)); free_argv(argv); return 1; }
    if (pid == 0) {
        restore_redirs();
        if (n->nredirs && apply_redirs(n) < 0) _exit(127);
        apply_assigns(n, 1);
        var_export_all();
        trace_command(argv);
        exec_external_cmd(argv);
    }
    free_argv(argv);
    int st;
    sigchld_block();
    waitpid(pid, &st, 0);
    sigchld_unblock();
    if (WIFEXITED(st)) g_status = WEXITSTATUS(st);
    else if (WIFSIGNALED(st)) { g_status = 128 + WTERMSIG(st); }
    else g_status = 128;
    return g_status;
}

/* ---------- pipelines ---------- */
static int exec_pipe(Node *n, int bg) {
    /* collect pipeline stages */
    Vec stages; vec_init(&stages);
    Node *cur = n;
    while (cur && cur->kind == N_PIPE) {
        vec_push(&stages, cur->b);
        cur = cur->a;
    }
    vec_push(&stages, cur);
    /* stages are in reverse order */
    int nstages = stages.len;
    int (*pipes)[2] = xmalloc(sizeof(int[2]) * (nstages - 1));
    for (int i = 0; i < nstages - 1; i++)
        if (pipe(pipes[i]) != 0) { fprintf(stderr, "osh: pipe: %s\n", strerror(errno)); return 1; }
    pid_t *pids = xmalloc(sizeof(pid_t) * nstages);
    for (int i = 0; i < nstages; i++) {
        pid_t pid = fork();
        if (pid < 0) { fprintf(stderr, "osh: fork: %s\n", strerror(errno)); return 1; }
        if (pid == 0) {
            /* child: wire up pipes */
            if (i > 0) dup2(pipes[i - 1][0], 0);
            if (i < nstages - 1) dup2(pipes[i][1], 1);
            for (int k = 0; k < nstages - 1; k++) { close(pipes[k][0]); close(pipes[k][1]); }
            /* run this stage in-process if it is a simple command */
            Node *stage = (Node *)vec_at(&stages, nstages - 1 - i);
            if (stage->kind == N_CMD && is_builtin_safe(stage)) {
                int argc;
                char **argv = build_argv(stage, &argc);
                if (apply_redirs(stage) < 0) _exit(1);
                trace_command(argv);
                int rc = run_builtin(argc, argv);
                fflush(NULL);
                _exit(rc);
            }
            if (stage->kind == N_CMD) {
                int argc;
                char **argv = build_argv(stage, &argc);
                if (apply_redirs(stage) < 0) _exit(127);
                apply_assigns(stage, 1);
                var_export_all();
                trace_command(argv);
                exec_external_cmd(argv);
            }
            exec_node(stage, 0);
            _exit(g_status);
        }
        pids[i] = pid;
    }
    for (int k = 0; k < nstages - 1; k++) { close(pipes[k][0]); close(pipes[k][1]); }
    int rc = 0;
    if (bg) {
        g_last_bg = pids[nstages - 1];
        job_add(pids[nstages - 1], n);
        printf("[%d] %d\n", next_job - 1, (int)g_last_bg);
    } else {
        sigchld_block();
        for (int i = 0; i < nstages; i++) {
            int st;
            waitpid(pids[i], &st, 0);
            if (i == nstages - 1) {
                if (WIFEXITED(st)) rc = WEXITSTATUS(st);
                else if (WIFSIGNALED(st)) rc = 128 + WTERMSIG(st);
                else rc = 128;
            }
        }
        sigchld_unblock();
    }
    free(pipes);
    free(pids);
    free(stages.data);      /* node pointers are owned by the caller */
    g_status = rc;
    return rc;
}

/* is it safe to run this builtin in a pipeline child? */
/* true only for real builtins that do not depend on shared shell state,
 * so they can run in a forked pipeline stage */
int is_builtin_safe(Node *n) {
    if (n->kind != N_CMD || !n->nargs) return 0;
    Vec v; vec_init(&v);
    expand_word(&n->args[0], &v, 0);
    int safe = 0;
    if (v.len) {
        const char *name = (const char *)vec_at(&v, 0);
        if (!strcmp(name, "echo") || !strcmp(name, "printf") ||
            !strcmp(name, "true") || !strcmp(name, "false") || !strcmp(name, "pwd"))
            safe = 1;
    }
    vec_free(&v);
    return safe;
}

/* ---------- compound commands ---------- */
static int exec_list(Node *n);   /* forward */

static int exec_compound(Node *n, int bg) {
    switch (n->kind) {
    case N_NONE: return 0;
    case N_CMD:  return exec_simple(n);
    case N_PIPE: return exec_pipe(n, bg);
    case N_AND:
        exec_node(n->a, 0);
        if (g_flow != FLOW_NONE) return g_status;
        if (g_status == 0) return exec_node(n->b, 0);
        return g_status;
    case N_OR:
        exec_node(n->a, 0);
        if (g_flow != FLOW_NONE) return g_status;
        if (g_status != 0) return exec_node(n->b, 0);
        return g_status;
    case N_SEQ:
        exec_node(n->a, 0);
        if (g_flow != FLOW_NONE) return g_status;
        return exec_node(n->b, 0);
    case N_BG: {
        pid_t pid = fork();
        if (pid < 0) return 1;
        if (pid == 0) {
            g_interactive = 0;
            shell_signals_init();
            exec_node(n->a, 0);
            _exit(g_status);
        }
        g_last_bg = pid;
        int slot = job_add(pid, n->a);
        printf("[%d] %d\n", jobs[slot].job, (int)pid);
        return 0;
    }
    case N_SUB: case N_GROUP: {
        int saved0 = dup(0), saved1 = dup(1), saved2 = dup(2);
        n_saved = 0;
        int rc;
        if (apply_redirs(n) < 0) rc = 1;
        else if (n->kind == N_SUB) {
            pid_t pid = fork();
            if (pid == 0) {
                g_interactive = 0;
                shell_signals_init();
                exec_node(n->a, 0);
                _exit(g_status);
            }
            int st; waitpid(pid, &st, 0);
            rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128;
            g_status = rc;
        } else {
            rc = exec_node(n->a, 0);
            g_status = rc;
        }
        restore_redirs();
        dup2(saved0, 0); dup2(saved1, 1); dup2(saved2, 2);
        close(saved0); close(saved1); close(saved2);
        return rc;
    }
    case N_IF: {
        int s0 = dup(0), s1 = dup(1), s2 = dup(2);
        n_saved = 0;
        int rc;
        if (apply_redirs(n) < 0) rc = 1;
        else {
            exec_node(n->a, 0);
            if (g_flow != FLOW_NONE) rc = g_status;
            else if (g_status == 0) rc = exec_node(n->b, 0);
            else if (n->c) rc = exec_node(n->c, 0);
            else rc = 0;
        }
        restore_redirs();
        dup2(s0, 0); dup2(s1, 1); dup2(s2, 2);
        close(s0); close(s1); close(s2);
        return rc;
    }
    case N_WHILE: case N_UNTIL: {
        int s0 = dup(0), s1 = dup(1), s2 = dup(2);
        n_saved = 0;
        int rc = 0, is_while = (n->kind == N_WHILE);
        if (apply_redirs(n) < 0) rc = 1;
        else for (;;) {
            exec_node(n->a, 0);
            if (g_flow != FLOW_NONE) break;
            if (is_while ? (g_status != 0) : (g_status == 0)) break;
            rc = exec_node(n->b, 0);
            if (g_flow != FLOW_NONE) break;
        }
        if (g_flow == FLOW_BREAK || g_flow == FLOW_CONTINUE) g_flow = FLOW_NONE;
        restore_redirs();
        dup2(s0, 0); dup2(s1, 1); dup2(s2, 2);
        close(s0); close(s1); close(s2);
        g_status = rc;
        return rc;
    }
    case N_FOR: {
        int s0 = dup(0), s1 = dup(1), s2 = dup(2);
        n_saved = 0;
        int rc = 0;
        if (apply_redirs(n) < 0) rc = 1;
        else {
            Vec values; vec_init(&values);
            if (n->nargs_set) {
                for (int i = 0; i < n->nargs; i++)
                    expand_word(&n->args[i], &values, EX_SPLIT | EX_GLOB);
            } else {
                for (int i = 0; i < g_nposargs; i++)
                    vec_push(&values, xstrdup(g_posargs[i]));
            }
            for (int i = 0; i < values.len; i++) {
                var_set(n->var, (char *)vec_at(&values, i));
                rc = exec_node(n->a, 0);
                if (g_flow == FLOW_BREAK) { g_flow = FLOW_NONE; break; }
                if (g_flow == FLOW_CONTINUE) { g_flow = FLOW_NONE; continue; }
                if (g_flow == FLOW_RETURN) break;
            }
            vec_free(&values);
        }
        restore_redirs();
        dup2(s0, 0); dup2(s1, 1); dup2(s2, 2);
        close(s0); close(s1); close(s2);
        return rc;
    }
    case N_CASE: {
        int s0 = dup(0), s1 = dup(1), s2 = dup(2);
        n_saved = 0;
        int rc;
        if (apply_redirs(n) < 0) rc = 1;
        else {
            Vec v; vec_init(&v);
            expand_word(&n->args[0], &v, EX_SPLIT | EX_GLOB);
            char *word = v.len ? xstrdup((char *)vec_at(&v, 0)) : xstrdup("");
            vec_free(&v);
            rc = 0;
            for (Node *b = n->a; b; b = b->b) {
                for (int i = 0; i < b->nargs; i++) {
                    Vec pv; vec_init(&pv);
                    expand_word(&b->args[i], &pv, 0);
                    char *pat = pv.len ? xstrdup((char *)vec_at(&pv, 0)) : xstrdup("");
                    vec_free(&pv);
                    int hit = gmatch_c(word, pat);
                    free(pat);
                    if (hit) { rc = exec_node(b->a, 0); break; }
                }
                if (g_flow != FLOW_NONE) break;
            }
            free(word);
        }
        restore_redirs();
        dup2(s0, 0); dup2(s1, 1); dup2(s2, 2);
        close(s0); close(s1); close(s2);
        return rc;
    }
    case N_ARITH_FOR: {
        /* for (( init; cond; step )) */
        int s0 = dup(0), s1 = dup(1), s2 = dup(2);
        n_saved = 0;
        int rc = 0;
        if (apply_redirs(n) < 0) rc = 1;
        else {
            if (n->var && *n->var) arith_eval(n->var);
            for (;;) {
                if (n->cond && *n->cond) {
                    long long v = arith_eval(n->cond);
                    if (!v) break;
                }
                rc = exec_node(n->a, 0);
                if (g_flow == FLOW_BREAK) { g_flow = FLOW_NONE; break; }
                if (g_flow == FLOW_CONTINUE) { g_flow = FLOW_NONE; }
                if (g_flow == FLOW_RETURN) break;
                if (n->step && *n->step) arith_eval(n->step);
            }
        }
        restore_redirs();
        dup2(s0, 0); dup2(s1, 1); dup2(s2, 2);
        close(s0); close(s1); close(s2);
        return rc;
    }
    case N_TEST: {
        /* [[ ... ]]: expand args without splitting/globbing, then evaluate */
        Vec argv; vec_init(&argv);
        for (int i = 0; i < n->nargs; i++) {
            Vec wv; vec_init(&wv);
            expand_word(&n->args[i], &wv, 0);
            if (wv.len) vec_push(&argv, xstrdup((char *)wv.data[0]));
            else vec_push(&argv, xstrdup(""));
            vec_free(&wv);
        }
        int argc = (int)argv.len + 2;
        char **av = xmalloc(sizeof(char *) * (argc + 1));
        av[0] = xstrdup("test");
        for (int i = 0; i < (int)argv.len; i++) av[i + 1] = xstrdup((char *)argv.data[i]);
        av[argc - 1] = xstrdup("]");
        av[argc] = 0;
        int rc = test_eval(argc, av) ? 0 : 1;
        for (int i = 0; i < argc; i++) free(av[i]);
        free(av); vec_free(&argv);
        g_status = rc;
        return rc;
    }
    default: return 0;
    }
}

int exec_node(Node *n, int bg) {
    if (!n) return 0;
    int rc = exec_compound(n, bg);
    if (g_opt_errexit && rc != 0 && !bg && g_flow == FLOW_NONE) {
        if (g_exit_jmp_set) longjmp(g_exit_jmp, 1);
    }
    return rc;
}

static int exec_list(Node *n) { return exec_node(n, 0); }

/* run shell source text in the current process */
void run_string(const char *code) {
    Reader r; reader_init_buf(&r, code);
    Lexer lx; lex_init(&lx, &r);
    for (;;) {
        Node *n = parse_line(&lx);
        if (!n) break;
        exec_node(n, 0);
        node_free(n);
        if (g_flow != FLOW_NONE) break;
    }
}

/* render a node back to text for job messages and xtrace */
void node_to_str(Node *n, Str *s) {
    if (!n) return;
    switch (n->kind) {
    case N_CMD:
        for (int i = 0; i < n->nargs; i++) {
            if (i) str_putc(s, ' ');
            char *raw = word_raw(&n->args[i]);
            str_puts(s, raw);
            free(raw);
        }
        break;
    case N_PIPE:
        node_to_str(n->a, s); str_puts(s, " | "); node_to_str(n->b, s);
        break;
    case N_AND:
        node_to_str(n->a, s); str_puts(s, " && "); node_to_str(n->b, s);
        break;
    case N_OR:
        node_to_str(n->a, s); str_puts(s, " || "); node_to_str(n->b, s);
        break;
    case N_SEQ:
        node_to_str(n->a, s); str_puts(s, "; "); node_to_str(n->b, s);
        break;
    default:
        str_puts(s, "...");
        break;
    }
}

/* render a node back to shell source text (used to store function bodies) */
/* re-add quotes so re-parsed function bodies keep quoting semantics */
static void word_to_source(Word *w, Str *s) {
    int prev_q = 0;
    for (int i = 0; i < w->n; i++) {
        int q = w->s[i].q;
        if (q && !prev_q) str_putc(s, '"');
        else if (!q && prev_q) str_putc(s, '"');
        const char *t = w->s[i].text;
        for (const char *p = t; *p; p++) {
            if (q && (*p == '"' || *p == '\\')) str_putc(s, '\\');
            str_putc(s, *p);
        }
        prev_q = q;
    }
    if (prev_q) str_putc(s, '"');
}

void node_to_source(Node *n, Str *s) {
    if (!n) return;
    switch (n->kind) {
    case N_CMD:
        for (int i = 0; i < n->nargs; i++) {
            if (i) str_putc(s, ' ');
            word_to_source(&n->args[i], s);
        }
        for (int i = 0; i < n->nredirs; i++) {
            Redir *r = &n->redirs[i];
            str_putc(s, ' ');
            switch (r->type) {
            case R_IN: str_puts(s, "<"); break;
            case R_OUT: str_puts(s, ">"); break;
            case R_APP: str_puts(s, ">>"); break;
            case R_ERR: str_puts(s, "2>"); break;
            case R_ERRAPP: str_puts(s, "2>>"); break;
            case R_HEREDOC: str_puts(s, "<<"); break;
            case R_DHEREDOC: str_puts(s, "<<-"); break;
            case R_HERESTR: str_puts(s, "<<<"); break;
            case R_DUPOUT: str_puts(s, ">&"); break;
            case R_DUPIN: str_puts(s, "<&"); break;
            case R_CLOBBER: str_puts(s, ">|"); break;
            case R_INOUT: str_puts(s, "<>"); break;
            }
            str_putc(s, ' ');
            word_to_source(&r->target, s);
        }
        break;
    case N_PIPE:
        node_to_source(n->a, s); str_puts(s, " | "); node_to_source(n->b, s);
        break;
    case N_AND:
        node_to_source(n->a, s); str_puts(s, " && "); node_to_source(n->b, s);
        break;
    case N_OR:
        node_to_source(n->a, s); str_puts(s, " || "); node_to_source(n->b, s);
        break;
    case N_SEQ:
        node_to_source(n->a, s); str_puts(s, "; "); node_to_source(n->b, s);
        break;
    case N_BG:
        node_to_source(n->a, s); str_puts(s, " &");
        break;
    case N_GROUP:
        str_puts(s, "{ "); node_to_source(n->a, s); str_puts(s, "; }");
        break;
    case N_SUB:
        str_puts(s, "( "); node_to_source(n->a, s); str_puts(s, " )");
        break;
    case N_IF:
        str_puts(s, "if "); node_to_source(n->a, s); str_puts(s, "; then ");
        node_to_source(n->b, s);
        if (n->c) { str_puts(s, "; else "); node_to_source(n->c, s); }
        str_puts(s, "; fi");
        break;
    case N_WHILE:
        str_puts(s, "while "); node_to_source(n->a, s); str_puts(s, "; do ");
        node_to_source(n->b, s); str_puts(s, "; done");
        break;
    case N_UNTIL:
        str_puts(s, "until "); node_to_source(n->a, s); str_puts(s, "; do ");
        node_to_source(n->b, s); str_puts(s, "; done");
        break;
    case N_FOR:
        str_puts(s, "for "); str_puts(s, n->var ? n->var : "");
        if (n->nargs_set) {
            str_puts(s, " in ");
            for (int i = 0; i < n->nargs; i++) {
                if (i) str_putc(s, ' ');
                char *raw = word_raw(&n->args[i]);
                str_puts(s, raw);
                free(raw);
            }
        }
        str_puts(s, "; do ");
        node_to_source(n->a, s);
        str_puts(s, "; done");
        break;
    default:
        str_puts(s, " ... ");
        break;
    }
}
