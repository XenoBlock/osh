/* match.c - pattern matching for the case command and [[ ]] tests */
#include "osh.h"

/* shell pattern match (same algorithm as the glob matcher, without escaping) */
int gmatch_c(const char *str, const char *pat) {
    while (*pat) {
        if (*pat == '*') {
            while (*pat == '*') pat++;
            if (!*pat) return 1;
            for (const char *s = str; *s; s++)
                if (gmatch_c(s, pat)) return 1;
            return gmatch_c(str, pat);
        }
        if (*pat == '?') {
            if (!*str) return 0;
            pat++; str++;
            continue;
        }
        if (*pat == '[') {
            const char *p = pat + 1;
            int negate = 0;
            if (*p == '!' || *p == '^') { negate = 1; p++; }
            int found = 0, ok = 0;
            while (*p && *p != ']') {
                if (p[1] == '-' && p[2] && p[2] != ']') {
                    if ((unsigned char)*str >= (unsigned char)p[0] &&
                        (unsigned char)*str <= (unsigned char)p[2]) found = 1;
                    p += 3;
                } else {
                    if (*p == *str) found = 1;
                    p++;
                }
                ok = 1;
            }
            if (!ok) return 0;
            if (*p == ']') p++;
            int m = negate ? !found : found;
            if (!m) return 0;
            pat = p;
            str++;
            continue;
        }
        if (*pat != *str) return 0;
        pat++; str++;
    }
    return *str == 0;
}

/* match a pattern as a prefix of str; return chars consumed, -1 if no match */
int match_prefix(const char *str, const char *pat) {
    if (!*pat) return 0;
    /* literal pattern: fast path */
    const char *p0 = pat;
    int star = 0;
    for (const char *q = pat; *q; q++) if (*q == '*' || *q == '?' || *q == '[') { star = 1; break; }
    if (!star) {
        size_t pl = strlen(pat);
        return strncmp(str, pat, pl) == 0 ? (int)pl : -1;
    }
    (void)p0;
    /* try increasing prefix lengths, prefer longest */
    size_t sl = strlen(str);
    for (size_t n = sl + 1; n-- > 0;) {
        char *tmp = xstrndup(str, n);
        int ok = gmatch_c(tmp, pat);
        free(tmp);
        if (ok) return (int)n;
    }
    return -1;
}

/* the [[ ... ]] conditional: strings and file tests */
static int str_op(const char *a, const char *op, const char *b) {
    if (!strcmp(op, "==") || !strcmp(op, "=")) return gmatch_c(a, b);
    if (!strcmp(op, "!=")) return !gmatch_c(a, b);
    if (!strcmp(op, "<"))  return strcmp(a, b) < 0;
    if (!strcmp(op, ">"))  return strcmp(a, b) > 0;
    if (!strcmp(op, "-eq")) return atoll(a) == atoll(b);
    if (!strcmp(op, "-ne")) return atoll(a) != atoll(b);
    if (!strcmp(op, "-lt")) return atoll(a) <  atoll(b);
    if (!strcmp(op, "-le")) return atoll(a) <= atoll(b);
    if (!strcmp(op, "-gt")) return atoll(a) >  atoll(b);
    if (!strcmp(op, "-ge")) return atoll(a) >= atoll(b);
    return 0;
}

static int unary_test(const char *op, const char *a) {
    if (!strcmp(op, "-z")) return !a || !*a;
    if (!strcmp(op, "-n")) return a && *a;
    if (!strcmp(op, "-e")) return test_file_op('e', a);
    if (!strcmp(op, "-f")) return test_file_op('f', a);
    if (!strcmp(op, "-d")) return test_file_op('d', a);
    if (!strcmp(op, "-r")) return test_file_op('r', a);
    if (!strcmp(op, "-w")) return test_file_op('w', a);
    if (!strcmp(op, "-x")) return test_file_op('x', a);
    if (!strcmp(op, "-s")) return test_file_op('s', a);
    return 0;
}

/* evaluate the argument list of `test`/`[` as a boolean */
int test_eval(int argc, char **argv) {
    /* strip the trailing ] when invoked as [ */
    int end = argc;
    if (argc > 1 && !strcmp(argv[argc - 1], "]")) end = argc - 1;
    int n = end - 1;
    if (n == 0) return 0;
    if (n == 1) return argv[1][0] != 0;
    if (n == 2) {
        if (argv[1][0] == '!' ) return !argv[2][0];
        return unary_test(argv[1], argv[2]);
    }
    if (n == 3) {
        if (!strcmp(argv[1], "!")) return !unary_test(argv[2], argv[3]);
        return str_op(argv[1], argv[2], argv[3]);
    }
    if (n == 4 && !strcmp(argv[1], "!"))
        return !str_op(argv[2], argv[3], argv[4]);
    /* longer expressions: left-to-right && / || handled by the caller */
    return 0;
}
