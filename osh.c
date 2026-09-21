/* osh.c - Oricade Shell: entry point, interactive loop, script mode, tests */
#include "osh.h"
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <pwd.h>

FILE *g_out = NULL;
static char *g_name = "osh";
static int g_opt_login = 0;

const char *shell_name(void) { return g_name; }

void osh_die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s: ", g_name);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

static void usage(void) {
    fputs(
"osh " OSH_VERSION " - Oricade Shell\n"
"Usage: osh [options] [command | script]\n"
"  -c cmd        run the string `cmd' and exit\n"
"  -s            read commands from stdin (default with no script file)\n"
"  -i            force interactive mode\n"
"  -l            login shell: read " RC_FILE " from the home directory\n"
"  -f            disable filename globbing\n"
"  -e            exit on the first failed command\n"
"  -u            treat unset variables as errors\n"
"  -x            print commands as they run (xtrace)\n"
"  -v            verbose: print input lines\n"
"  --version     print version and exit\n"
"  --help        print this help and exit\n"
"  --self-test   run the built-in test suite and exit\n"
"  --session ID  attach to (or start) shared session ID\n",
    stderr);
}

static void run_file(FILE *f) {
    Reader r; reader_init_file(&r, f);
    Lexer lx; lex_init(&lx, &r);
    for (;;) {
        Node *n = parse_line(&lx);
        if (!n) break;
        exec_node(n, 0);
        node_free(n);
        if (g_flow != FLOW_NONE) break;
    }
}

static void run_interactive(void) {
    edit_init();
    shell_signals_init();
    const char *ps1 = var_get("PS1");
    if (!ps1) { var_set("PS1", "\\u@\\h:\\w\\$ "); ps1 = var_get("PS1"); }
    const char *ps2 = var_get("PS2");
    if (!ps2) var_set("PS2", "> ");
    g_exit_jmp_set = 1;
    int code = setjmp(g_exit_jmp);
    if (code) exit(code);
    for (;;) {
        char *line = edit_getline(ps1);
        if (!line) { fputs("exit\n", stdout); break; }
        if (*line) edit_add_history(line);
        Reader r; reader_init_buf(&r, line);
        Lexer lx; lex_init(&lx, &r);
        for (;;) {
            Node *n = parse_line(&lx);
            if (!n) break;
            exec_node(n, 0);
            node_free(n);
            if (g_flow != FLOW_NONE) break;
        }
        free(line);
        jobs_print_changes();
    }
    edit_save_history();
}

static void load_rc(void) {
    Str path; str_init(&path);
    str_puts(&path, home_dir());
    str_putc(&path, '/');
    str_puts(&path, RC_FILE);
    FILE *f = fopen(path.buf, "r");
    if (f) { run_file(f); fclose(f); }
    str_free(&path);
}

/* ---------------- built-in self tests ---------------- */
static int tests_run = 0, tests_fail = 0;

static void check_int(const char *what, long long got, long long want) {
    tests_run++;
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %lld want %lld\n", what, got, want);
        tests_fail++;
    }
}

static void check_str(const char *what, const char *got, const char *want) {
    tests_run++;
    if (!got || strcmp(got, want)) {
        fprintf(stderr, "FAIL %s: got \"%s\" want \"%s\"\n", what, got ? got : "(null)", want);
        tests_fail++;
    }
}

static void check_status(const char *code, int want) {
    tests_run++;
    run_string(code);
    if (g_status != want) {
        fprintf(stderr, "FAIL [%s]: status %d want %d\n", code, g_status, want);
        tests_fail++;
    }
}

static void test_arith(void) {
    check_int("1+1", arith_eval("1+1"), 2);
    check_int("2*3+4", arith_eval("2*3+4"), 10);
    check_int("(2+3)*4", arith_eval("(2+3)*4"), 20);
    check_int("10%3", arith_eval("10%3"), 1);
    check_int("1<<4", arith_eval("1<<4"), 16);
    check_int("5>3", arith_eval("5>3"), 1);
    check_int("5<3", arith_eval("5<3"), 0);
    check_int("5==5", arith_eval("5==5"), 1);
    check_int("5!=5", arith_eval("5!=5"), 0);
    check_int("7&5", arith_eval("7&5"), 5);
    check_int("7|8", arith_eval("7|8"), 15);
    check_int("7^5", arith_eval("7^5"), 2);
    check_int("~-5", arith_eval("~-5"), 4);
    check_int("!0", arith_eval("!0"), 1);
    check_int("1?2:3", arith_eval("1?2:3"), 2);
    check_int("0?2:3", arith_eval("0?2:3"), 3);
    check_int("2>1", arith_eval("2>1"), 1);
    check_int("- 5", arith_eval("- 5"), -5);
}

static void test_vars(void) {
    var_set("x", "hello");
    check_str("var_get", var_get("x"), "hello");
    run_string("y=42");
    check_str("assignment", var_get("y"), "42");
    run_string("z=$((6*7))");
    check_str("arith assign", var_get("z"), "42");
    run_string("a=1; b=2; c=$((a+b))");
    check_str("arith vars", var_get("c"), "3");
    run_string("d=${x:-default}");
    check_str("param default used", var_get("d"), "hello");
    run_string("e=${unset_x:-default}");
    check_str("param default", var_get("e"), "default");
    run_string("f=${x:+set}");
    check_str("param alt", var_get("f"), "set");
    run_string("g=${#x}");
    check_str("param length", var_get("g"), "5");
    run_string("h=$(echo nested)");
    check_str("command sub", var_get("h"), "nested");
    run_string("i=`echo backtick`");
    check_str("backticks", var_get("i"), "backtick");
}

static void test_glob(void) {
    check_int("gmatch literal", gmatch_c("abc", "abc"), 1);
    check_int("gmatch star", gmatch_c("abc", "a*"), 1);
    check_int("gmatch star empty", gmatch_c("", "a*"), 0);
    check_int("gmatch question", gmatch_c("abc", "a?c"), 1);
    check_int("gmatch class", gmatch_c("abc", "a[bd]c"), 1);
    check_int("gmatch negclass", gmatch_c("abc", "a[!de]c"), 1);
    check_int("gmatch nomatch", gmatch_c("abc", "a[de]c"), 0);
    check_int("gmatch whole", gmatch_c("file.txt", "*.txt"), 1);
}

static void test_flow(void) {
    check_status("true", 0);
    check_status("false", 1);
    check_status("if true; then true; fi", 0);
    check_status("if false; then true; else false; fi", 1);
    check_status("if false; then a; elif true; then true; fi", 0);
    check_status("while false; do true; done", 0);
    check_status("for i in 1 2 3; do true; done", 0);
    check_status("case a in a) true;; b) false;; esac", 0);
    check_status("case b in a) true;; b) false;; esac", 1);
    check_status("true && true", 0);
    check_status("true && false", 1);
    check_status("false || true", 0);
    check_status("false || false", 1);
    check_status("{ true; }", 0);
    check_status("( false )", 1);
    check_status("a=1; [ $a = 1 ]", 0);
    check_status("[ -d / ]", 0);
    check_status("[ -f /etc/hostname ]", 0);
    check_status("[ 5 -gt 3 ]", 0);
    check_status("[ 5 -lt 3 ]", 1);
    check_status("[ -z ]", 0);
    check_status("f() { echo hi; }; f", 0);
}

static void test_builtins(void) {
    check_status("echo hello", 0);
    check_status("echo -n hi", 0);
    check_status("cd / && pwd", 0);
    check_status("cd /nonexistent-dir-xyz", 1);
    check_status("type echo", 0);
    check_status("alias xx='echo aliased'; type xx", 0);
}

static void test_expansion(void) {
    run_string("list=\"a b c\"; n=0; for x in $list; do n=$((n+1)); done");
    check_str("word splitting count", var_get("n"), "3");
    run_string("n=$((1+1))");
    check_str("dollar paren arith", var_get("n"), "2");
    run_string("s='single'; echo $s >/dev/null");
    check_str("single quotes", var_get("s"), "single");
}

static void test_param_expansion(void) {
    run_string("s=hello");
    check_str("${var:1:3}", expand_param_str("s:1:3"), "ell");
    check_str("${var:2}", expand_param_str("s:2"), "llo");
    check_str("${var: -2}", expand_param_str("s: -2"), "lo");
    check_str("${#var}", expand_param_str("#s"), "5");
    check_str("${var/llo/LLO}", expand_param_str("s/llo/LLO"), "heLLO");
    check_str("${var//l/L}", expand_param_str("s//l/L"), "heLLo");
    check_str("${var/h*/H}", expand_param_str("s/h*/H"), "H");
    check_str("${X:-def}", expand_param_str("X:-def"), "def");
    run_string("X=; ${X:=set}");
    check_str("${X:=set}", var_get("X"), "set");
}

/* options that exist for safety, and the session id -> file name check */
static void test_hardening(void) {
    char tmpl[] = "/tmp/osh_selftest_XXXXXX";
    char *dir = mkdtemp(tmpl);
    char *cwd = getcwd(NULL, 0);
    if (!dir || chdir(dir) != 0) { free(cwd); return; }

    run_string("echo one > f");
    check_status("set -C; echo two > f", 1);
    char kept[8] = {0};
    FILE *fp = fopen("f", "r");
    if (fp) { size_t n = fread(kept, 1, sizeof kept - 1, fp); kept[n] = 0; fclose(fp); }
    check_str("noclobber keeps content", kept, "one\n");
    check_status("set -C; echo three >| f", 0);
    check_status("set -C; echo fresh > new", 0);
    run_string("set +C");
    check_int("set +C clears noclobber", g_opt_noclobber, 0);
    check_status("echo four > f", 0);

    /* an unknown job must not fall through to kill(-1, sig) */
    check_status("kill %999", 1);
    check_status("kill -s 999 $$", 1);
    check_status("kill -s", 1);

    check_int("session id plain", session_id_ok("work"), 1);
    check_int("session id punctuation", session_id_ok("a-b_1.2"), 1);
    check_int("session id traversal", session_id_ok("../evil"), 0);
    check_int("session id slash", session_id_ok("a/b"), 0);
    check_int("session id empty", session_id_ok(""), 0);
    check_int("session id dotdot", session_id_ok(".."), 0);
    check_int("session id too long", session_id_ok(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), 0);

    /* completion sources */
    var_set("OSHZZFOO", "1");
    var_set("OSHZZBAR", "2");
    Vec v; vec_init(&v);
    var_names_matching("OSHZZ", &v);
    check_int("var completion matches", v.len, 2);
    vec_free(&v);

    if (cwd) { chdir(cwd); free(cwd); }
    unlink("f"); unlink("new");
    rmdir(dir);
}

static void run_self_test(void) {
    fprintf(stderr, "osh " OSH_VERSION " self-test\n");
    test_arith();
    test_vars();
    test_glob();
    test_flow();
    test_builtins();
    test_expansion();
    test_param_expansion();
    test_hardening();
    fprintf(stderr, "%d tests, %d failures\n", tests_run, tests_fail);
    exit(tests_fail ? 1 : 0);
}

/* ---------------- main ---------------- */
int main(int argc, char **argv) {
    g_out = stdout;
    g_arg0 = xstrdup(argv[0]);
    const char *cmd = NULL;
    int opt_s = 0;
    int selftest = 0;
    const char *session = NULL;

    setenv("SHELL", "osh", 0);
    var_import_env();
    var_set("PS1", "\\u@\\h:\\w\\$ ");
    var_set("PS2", "> ");
    var_set("IFS", " \t\n");
    g_ifs = xstrdup(var_get("IFS"));

    int i = 1;
    for (; i < argc; i++) {
        if (argv[i][0] != '-') break;
        if (!strcmp(argv[i], "--")) { i++; break; }
        if (!strcmp(argv[i], "--version")) {
            printf("osh " OSH_VERSION " (Oricade Shell)\n");
            return 0;
        }
        if (!strcmp(argv[i], "--help")) { usage(); return 0; }
        if (!strcmp(argv[i], "--self-test")) { selftest = 1; continue; }
        if (!strcmp(argv[i], "--session")) {
            if (i + 1 >= argc) osh_die("--session: option requires an argument");
            session = argv[++i];
            continue;
        }
        if (!strcmp(argv[i], "-c")) {
            if (i + 1 >= argc) osh_die("-c: option requires an argument");
            cmd = argv[++i];
            continue;
        }
        const char *o = argv[i] + 1;
        for (; *o; o++) {
            switch (*o) {
            case 's': opt_s = 1; break;
            case 'i': g_interactive = 1; break;
            case 'l': g_opt_login = 1; break;
            case 'f': g_opt_noglob = 1; break;
            case 'e': g_opt_errexit = 1; break;
            case 'u': g_opt_unset = 1; break;
            case 'x': g_opt_xtrace = 1; break;
            case 'v': g_opt_verbose = 1; break;
            default:  usage(); exit(2);
            }
        }
    }
    if (selftest) run_self_test();

    if (session) {
        /* the session server inherits this process's shell state */
        load_rc();
        return session_client(session);
    }

    if (cmd) {
        g_interactive = 0;
        run_string(cmd);
        trap_run(0);
        return g_status;
    }
    if (i < argc) {
        /* script file */
        FILE *f = fopen(argv[i], "r");
        if (!f) osh_die("%s: %s", argv[i], strerror(errno));
        g_arg0 = xstrdup(argv[i]);
        /* remaining args become positional parameters */
        g_nposargs = argc - i - 1;
        g_posargs = xmalloc(sizeof(char *) * (g_nposargs ? g_nposargs : 1));
        for (int k = 0; k < g_nposargs; k++) g_posargs[k] = xstrdup(argv[i + 1 + k]);
        shell_signals_init();
        run_file(f);
        trap_run(0);
        fclose(f);
        return g_status;
    }
    g_interactive = isatty(0) && (opt_s || isatty(1)) ? 1 : 0;
    if (g_opt_login || g_interactive) load_rc();
    if (g_interactive) run_interactive();
    else {
        shell_signals_init();
        run_file(stdin);
    }
    trap_run(0);
    return g_status;
}
