/* input.c - line reading for the `read` builtin and non-interactive input */
#include "osh.h"
#include <errno.h>

int read_input_line(Str *out) {
    str_clear(out);
    int c = getchar();
    if (c == EOF) return 0;
    while (c != EOF && c != '\n') { str_putc(out, (char)c); c = getchar(); }
    return 1;
}
