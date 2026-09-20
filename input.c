/* input.c - line reading for the `read` builtin and non-interactive input */
#include "osh.h"
#include <errno.h>

int read_input_line(Str *out) {
    str_clear(out);
    /* ponytail: read(2) directly. stdio's getchar() buffers fd 0, so it
       misses dup2'd heredoc/pipe input and can silently return stale EOF. */
    char c;
    ssize_t n = read(0, &c, 1);
    if (n <= 0) return 0;
    while (n > 0 && c != '\n') {
        str_putc(out, c);
        n = read(0, &c, 1);
    }
    return 1;
}
