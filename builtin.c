/* builtin.c - built-in commands */
#include "osh.h"
#include <errno.h>
#include <ctype.h>
#include <pwd.h>
#include <fcntl.h>
#include <sys/times.h>
#include <sys/utsname.h>

static int b_echo(int argc, char **argv) {
    int nflag = 0, eflag = 0;
    int i = 1;
    /* osh echo accepts -n and -e like bash */
    while (i < argc && argv[i][0] == '-' && argv[i][1] != 0) {
        if (!strcmp(argv[i], "-n")) { nflag = 1; i++; continue; }
        if (!strcmp(argv[i], "-e")) { eflag = 1; i++; continue; }
        if (!strcmp(argv[i], "-E")) { eflag = 0; i++; continue; }
        if (!strcmp(argv[i], "-ne") || !strcmp(argv[i], "-en")) { nflag = eflag = 1; i++; continue; }
        break;
    }
    for (; i < argc; i++) {
        if (eflag) {
            for (const char *p = argv[i]; *p; p++) {
                if (*p == '\\' && p[1]) {
                    switch (*++p) {
                    case 'n': putchar('\n'); break;
                    case 't': putchar('\t'); break;
                    case 'r': putchar('\r'); break;
                    case '\\': putchar('\\'); break;
                    case 'a': putchar('\a'); break;
                    case 'b': putchar('\b'); break;
                    case 'f': putchar('\f'); break;
                    case 'v': putchar('\v'); break;
                    case '0': case 'x': {
                        int base = (*p == '0') ? 8 : 16;
                        char *end;
                        int c = (int)strtol(p + 1, &end, base);
                        if (end != p + 1) { putchar((char)c); p = end - 1; }
                        else putchar(*p);
                        break;
                    }
                    default: putchar('\\'); putchar(*p); break;
                    }
                } else putchar(*p);
            }
        } else fputs(argv[i], stdout);
        if (i + 1 < argc) putchar(' ');
    }
    if (!nflag) putchar('\n');
    return 0;
}

static int b_true(int argc, char **argv) { (void)argc; (void)argv; return 0; }
static int b_false(int argc, char **argv) { (void)argc; (void)argv; return 1; }

static int b_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[4096];
    if (getcwd(buf, sizeof buf)) { printf("%s\n", buf); return 0; }
    fprintf(stderr, "osh: pwd: %s\n", strerror(errno));
    return 1;
}

static int b_cd(int argc, char **argv) {
    const char *dir = NULL;
    if (argc < 2) dir = var_get("HOME");
    else if (!strcmp(argv[1], "-")) dir = var_get("OLDPWD");
    else dir = argv[1];
    if (!dir || !*dir) dir = home_dir();
    if (chdir(dir) != 0) {
        fprintf(stderr, "osh: cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    char buf[4096];
    if (getcwd(buf, sizeof buf)) {
        var_set("OLDPWD", var_get("PWD") ? var_get("PWD") : "");
        var_set("PWD", buf);
    }
    return 0;
}

static int b_export(int argc, char **argv) {
    if (argc == 1) {
        for (size_t i = map_next_used(&g_exported, 0); i < g_exported.cap;
             i = map_next_used(&g_exported, i + 1)) {
            const char *v = var_get(g_exported.keys[i]);
            printf("declare -x %s=\"%s\"\n", g_exported.keys[i], v ? v : "");
        }
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            *eq = 0;
            var_set(argv[i], eq + 1);
            var_mark_export(argv[i]);
            *eq = '=';
        } else {
            var_mark_export(argv[i]);
        }
    }
    return 0;
}

static int b_unset(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (argv[i][0] == '-' && argv[i][1] == 'f') {
            if (i + 1 < argc) map_del(&g_funs, argv[++i]);
        } else var_unset(argv[i]);
    return 0;
}

static int b_set(int argc, char **argv) {
    if (argc == 1) {
        for (size_t i = map_next_used(&g_vars, 0); i < g_vars.cap;
             i = map_next_used(&g_vars, i + 1))
            printf("%s=%s\n", g_vars.keys[i], g_vars.vals[i] ? g_vars.vals[i] : "");
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') set_shell_options_from(argv[i] + 1);
        else if (argv[i][0] == '+') { /* not implemented: leave as-is */ }
        else {
            char *eq = strchr(argv[i], '=');
            if (eq) { *eq = 0; var_set(argv[i], eq + 1); *eq = '='; }
        }
    }
    return 0;
}

static int b_alias(int argc, char **argv) {
    if (argc == 1) {
        for (size_t i = map_next_used(&g_aliases, 0); i < g_aliases.cap;
             i = map_next_used(&g_aliases, i + 1))
            printf("alias %s='%s'\n", g_aliases.keys[i], g_aliases.vals[i]);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) {
            const char *v = map_get(&g_aliases, argv[i]);
            if (v) printf("alias %s='%s'\n", argv[i], v);
            else { fprintf(stderr, "osh: alias: %s: not found\n", argv[i]); return 1; }
        } else {
            *eq = 0;
            map_put(&g_aliases, argv[i], eq + 1);
            *eq = '=';
        }
    }
    return 0;
}

static int b_unalias(int argc, char **argv) {
    for (int i = 1; i < argc; i++) map_del(&g_aliases, argv[i]);
    return 0;
}

static int b_type(int argc, char **argv) {
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (map_get(&g_funs, argv[i])) printf("%s is a function\n", argv[i]);
        else if (is_builtin(argv[i])) printf("%s is a shell builtin\n", argv[i]);
        else if (map_get(&g_aliases, argv[i])) printf("%s is an alias to %s\n", argv[i], map_get(&g_aliases, argv[i]));
        else {
            char *p = command_resolve(argv[i]);
            if (p) { printf("%s is %s\n", argv[i], p); free(p); }
            else { fprintf(stderr, "osh: type: %s: not found\n", argv[i]); rc = 1; }
        }
    }
    return rc;
}

static int b_which(int argc, char **argv) {
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        char *p = command_resolve(argv[i]);
        if (p) { printf("%s\n", p); free(p); }
        else rc = 1;
    }
    return rc;
}

static int b_source(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "osh: source: filename argument required\n"); return 1; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { fprintf(stderr, "osh: %s: %s\n", argv[1], strerror(errno)); return 1; }
    Reader r; reader_init_file(&r, f);
    Lexer lx; lex_init(&lx, &r);
    int rc = 0;
    for (;;) {
        Node *n = parse_line(&lx);
        if (!n) break;
        rc = exec_node(n, 0);
        node_free(n);
        if (g_flow != FLOW_NONE) break;
    }
    fclose(f);
    return rc;
}

static int b_history(int argc, char **argv) {
    (void)argc; (void)argv;
    history_print();
    return 0;
}

static int b_jobs(int argc, char **argv) {
    (void)argc; (void)argv;
    jobs_print_all();
    return 0;
}

static int b_fg(int argc, char **argv) {
    (void)argc; (void)argv;
    fg_job(-1);
    return g_status;
}

static int b_bg(int argc, char **argv) {
    (void)argc; (void)argv;
    bg_job(-1);
    return 0;
}

static int b_kill(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "osh: kill: usage: kill [-s sig] pid|job\n"); return 1; }
    int sig = SIGTERM;
    int i = 1;
    if (argv[i][0] == '-' && argv[i][1] == 's') { sig = atoi(argv[++i]); i++; }
    else if (argv[i][0] == '-') { sig = atoi(argv[i] + 1); i++; }
    for (; i < argc; i++) {
        pid_t pid;
        if (argv[i][0] == '%') pid = job_pid(atoi(argv[i] + 1));
        else pid = atoi(argv[i]);
        if (kill(pid, sig) != 0) { fprintf(stderr, "osh: kill: %s\n", strerror(errno)); return 1; }
    }
    return 0;
}

static int b_wait(int argc, char **argv) {
    (void)argc; (void)argv;
    wait_for_jobs();
    return g_status;
}

static int b_trap(int argc, char **argv) {
    if (argc < 2) {
        for (size_t i = map_next_used(&g_traps, 0); i < g_traps.cap;
             i = map_next_used(&g_traps, i + 1))
            printf("trap -- '%s' %s\n", g_traps.vals[i], g_traps.keys[i]);
        return 0;
    }
    if (argc == 2) { trap_set(argv[1], ""); return 0; }
    trap_set(argv[2], argv[1]);
    return 0;
}

static int b_return(int argc, char **argv) {
    if (argc > 1) g_status = atoi(argv[1]);
    g_flow = FLOW_RETURN;
    return g_status;
}

static int b_break(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1;
    (void)n;
    g_flow = FLOW_BREAK;
    return 0;
}

static int b_continue(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1;
    (void)n;
    g_flow = FLOW_CONTINUE;
    return 0;
}

static int b_exit(int argc, char **argv) {
    int code = argc > 1 ? atoi(argv[1]) : g_status;
    if (g_exit_jmp_set) longjmp(g_exit_jmp, code);
    exit(code);
}

static int b_read(int argc, char **argv) {
    int from_stdin = 1;
    int i = 1;
    char *prompt = NULL;
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "-p")) { prompt = argv[++i]; i++; continue; }
        if (!strcmp(argv[i], "-r")) { i++; continue; }
        break;
    }
    Str line; str_init(&line);
    if (from_stdin) {
        if (prompt && g_interactive) { fputs(prompt, stderr); fflush(stderr); }
        if (!read_input_line(&line)) { str_free(&line); return 1; }
    }
    /* split the line into the given names using IFS */
    const char *ifs = (g_ifs && *g_ifs) ? g_ifs : " \t\n";
    const char *s = line.buf ? line.buf : "";
    int nnames = argc - i;
    int cur_name = 0;
    Str val; str_init(&val);
    while (*s) {
        if (strchr(ifs, *s)) {
            if (cur_name < nnames - 1) {
                if (i + cur_name < argc) { char *sv = str_done(&val); var_set(argv[i + cur_name], sv); free(sv); }
                cur_name++;
                str_clear(&val);
            } else str_putc(&val, *s);
        } else str_putc(&val, *s);
        s++;
    }
    if (i + cur_name < argc) { char *sv = str_done(&val); var_set(argv[i + cur_name], sv); free(sv); }
    if (nnames == 0) { char *sv = str_done(&val); var_set("REPLY", sv); free(sv); }
    str_free(&val);
    str_free(&line);
    return 0;
}

static int b_printf(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "osh: printf: usage: printf fmt [args]\n"); return 1; }
    const char *fmt = argv[1];
    int argi = 2;
    Str out; str_init(&out);
    for (const char *p = fmt; *p; p++) {
        if (*p == '\\' && p[1]) {
            switch (*++p) {
            case 'n': str_putc(&out, '\n'); break;
            case 't': str_putc(&out, '\t'); break;
            case 'r': str_putc(&out, '\r'); break;
            case '\\': str_putc(&out, '\\'); break;
            default: str_putc(&out, '\\'); str_putc(&out, *p); break;
            }
        } else if (*p == '%') {
            p++;
            if (!*p) { str_putc(&out, '%'); break; }
            const char *st = p;
            while (*p && strchr("0123456789.-", *p)) p++;
            char conv = *p;
            char fmtbuf[64];
            size_t fl = (size_t)(p - st) + 1;
            if (fl >= sizeof fmtbuf) fl = sizeof fmtbuf - 1;
            fmtbuf[0] = '%';
            memcpy(fmtbuf + 1, st, fl - 1);
            fmtbuf[fl] = 0;
            switch (conv) {
            case 'd': case 'i': {
                long long v = (argi < argc) ? atoll(argv[argi++]) : 0;
                Str t; str_init(&t); str_printf(&t, fmtbuf, v);
                str_puts(&out, t.buf); str_free(&t);
                break;
            }
            case 's': {
                const char *v = (argi < argc) ? argv[argi++] : "";
                Str t; str_init(&t); str_printf(&t, fmtbuf, v);
                str_puts(&out, t.buf); str_free(&t);
                break;
            }
            case 'x': case 'X': case 'o': case 'u': {
                long long v = (argi < argc) ? atoll(argv[argi++]) : 0;
                Str t; str_init(&t); str_printf(&t, fmtbuf, (unsigned long long)v);
                str_puts(&out, t.buf); str_free(&t);
                break;
            }
            case 'c': {
                const char *v = (argi < argc) ? argv[argi++] : "";
                str_putc(&out, v[0]);
                break;
            }
            case '%': str_putc(&out, '%'); break;
            default: str_putc(&out, '%'); str_putc(&out, conv); break;
            }
        } else str_putc(&out, *p);
    }
    fputs(out.buf ? out.buf : "", stdout);
    str_free(&out);
    return 0;
}

static int b_test(int argc, char **argv) {
    return test_eval(argc, argv) ? 0 : 1;
}

static int b_umask(int argc, char **argv) {
    if (argc < 2) {
        mode_t m = umask(0);
        printf("%04o\n", (unsigned)m);
        umask(m);
        return 0;
    }
    umask((mode_t)strtol(argv[1], NULL, 8));
    return 0;
}

static int b_times(int argc, char **argv) {
    (void)argc; (void)argv;
    struct tms t = {0};
    times(&t);
    double ticks = (double)sysconf(_SC_CLK_TCK);
    printf("%fm %fs\n%fm %fs\n", t.tms_utime / ticks * 60.0, t.tms_utime / ticks,
           t.tms_stime / ticks * 0, t.tms_stime / ticks);
    return 0;
}

static int b_help(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("osh " OSH_VERSION " - Oricade Shell\n");
    printf("Built-in commands:\n");
    for (Builtin *b = builtins; b->name; b++) printf("  %-12s ", b->name);
    printf("\n\nAlso supported: pipes, redirections (< > >> 2> << <<-, && || ; &),\n"
           "if/then/elif/else/fi, while, until, for, case, functions, aliases,\n"
           "$( ) command substitution, $(( )) arithmetic, ${ } parameter expansion.\n");
    return 0;
}

static int b_command(int argc, char **argv) {
    int i = 1;
    while (i < argc && argv[i][0] == '-') i++;
    if (i >= argc) return 0;
    if (is_builtin(argv[i])) return run_builtin(argc - i, argv + i);
    pid_t pid = fork();
    if (pid == 0) {
        execvp(argv[i], argv + i);
        fprintf(stderr, "osh: %s: %s\n", argv[i], strerror(errno));
        _exit(127);
    }
    int st; waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
}

static int b_eval(int argc, char **argv) {
    if (argc < 2) return 0;
    Str code; str_init(&code);
    for (int i = 1; i < argc; i++) {
        if (i > 1) str_putc(&code, ' ');
        str_puts(&code, argv[i]);
    }
    run_string(code.buf);
    str_free(&code);
    return g_status;
}

static int b_shift(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 1;
    if (n > g_nposargs) return 1;
    for (int i = 0; i < n; i++) free(g_posargs[i]);
    for (int i = 0; i + n < g_nposargs; i++) g_posargs[i] = g_posargs[i + n];
    g_nposargs -= n;
    return 0;
}

static int b_getopts(int argc, char **argv) {
    (void)argc; (void)argv;
    return 1;
}

static int b_hash(int argc, char **argv) {
    (void)argc; (void)argv;
    return 0;
}

static int b_exec(int argc, char **argv) {
    if (argc < 2) return 0;
    var_export_all();
    execvp(argv[1], argv + 1);
    fprintf(stderr, "osh: exec: %s: %s\n", argv[1], strerror(errno));
    return 127;
}

static int b_colon(int argc, char **argv) { (void)argc; (void)argv; return 0; }
static int b_dot(int argc, char **argv) { return b_source(argc, argv); }

static int b_osh_setvars(int argc, char **argv) {
    if (argc < 2) {
        printf("autoopen: %s\n", g_opt_autoopen ? "on" : "off");
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];
        char *eq = strchr(arg, '=');
        char *key = arg;
        char *val = NULL;
        char kbuf[64] = {0};
        if (eq) {
            size_t klen = (size_t)(eq - arg);
            if (klen >= sizeof(kbuf)) klen = sizeof(kbuf) - 1;
            memcpy(kbuf, arg, klen);
            kbuf[klen] = 0;
            key = kbuf;
            val = eq + 1;
        } else if (i + 1 < argc && (argv[i + 1][0] != '-' && strchr(argv[i + 1], '=') == NULL)) {
            val = argv[++i];
        }

        if (!strcmp(key, "autoopen") || !strcmp(key, "auto_open") || !strcmp(key, "open_without_dot_slash")) {
            if (!val) {
                printf("autoopen: %s\n", g_opt_autoopen ? "on" : "off");
            } else if (!strcmp(val, "1") || !strcasecmp(val, "on") || !strcasecmp(val, "true") || !strcasecmp(val, "enable")) {
                g_opt_autoopen = 1;
            } else if (!strcmp(val, "0") || !strcasecmp(val, "off") || !strcasecmp(val, "false") || !strcasecmp(val, "disable")) {
                g_opt_autoopen = 0;
            } else {
                fprintf(stderr, "osh: osh_setvars: invalid value '%s' for autoopen (use on/off or 1/0)\n", val);
                return 1;
            }
        } else if (eq) {
            var_set(key, val);
        } else {
            fprintf(stderr, "osh: osh_setvars: unknown option '%s'\n", key);
            return 1;
        }
    }
    return 0;
}

Builtin builtins[] = {
    {"echo",    b_echo},    {"cd",      b_cd},
    {"pwd",     b_pwd},     {"true",    b_true},
    {"false",   b_false},   {"export",  b_export},
    {"unset",   b_unset},   {"set",     b_set},
    {"alias",   b_alias},   {"unalias", b_unalias},
    {"type",    b_type},    {"which",   b_which},
    {"source",  b_source},  {".",       b_dot},
    {"history", b_history}, {"jobs",    b_jobs},
    {"fg",      b_fg},      {"bg",      b_bg},
    {"kill",    b_kill},    {"wait",    b_wait},
    {"trap",    b_trap},    {"return",  b_return},
    {"break",   b_break},   {"continue",b_continue},
    {"exit",    b_exit},    {"read",    b_read},
    {"printf",  b_printf},  {"test",    b_test},
    {"[",       b_test},    {"umask",   b_umask},
    {"times",   b_times},   {"help",    b_help},
    {"command", b_command}, {"eval",    b_eval},
    {"shift",   b_shift},   {"getopts", b_getopts},
    {"hash",    b_hash},    {"exec",    b_exec},
    {"osh_setvars", b_osh_setvars},
    {":",       b_colon},
    {NULL,      NULL}
};

int run_builtin(int argc, char **argv) {
    for (Builtin *b = builtins; b->name; b++)
        if (!strcmp(b->name, argv[0])) return b->fn(argc, argv);
    return 127;
}
