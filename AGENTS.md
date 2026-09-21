# AGENTS.md

Guidance for AI coding agents working on this repository.

## What this is

`osh` (Oricade Shell) is a lightweight POSIX-ish shell written in pure C11.
It is intentionally dependency-free: only the C standard library and POSIX
system calls. Do not propose third-party libraries.

## Build and verify

```bash
make                 # build ./osh
make check           # run ./osh --self-test
```

`--self-test` is the single runnable check for all non-trivial logic. After
any change to lexer, parser, expansion, or execution: rebuild, run
`./osh --self-test`, and confirm `0 failures`. Add a self-test case for any
new observable behavior.

Quick manual smoke (non-interactive, no tty needed):

```bash
printf 'echo hi | cat\nx=5; echo "$((x*2))"\n' | ./osh
```

## Architecture (data flow)

```
input.c/Reader  -->  lex.c (Lexer, Token, word segments with quote mask)
                -->  parse.c (recursive descent -> Node AST)
                -->  exec.c (exec_node dispatch: pipelines, compounds)
                -->  var.c (expand_word: params, cmdsub, split, glob)
```

`session.c` is a separate entry point: `osh --session ID` forks a server that
owns the shell state and runs commands with fd 0/1/2 pointed at whichever
client is currently attached. Clients are byte relays; takeover is a control
line (leading `0x01`) sent between commands.

Key invariants that are easy to break:

- **Quote mask (`q`)**: every word segment carries `0` (unquoted), `1`
  (single-quoted), or `2` (result of expansion). Word splitting and globbing
  only apply to segments with `q == 0`. If a new expansion path forgets to set
  the mask, unquoted globs start expanding inside quotes.
- **`Node` ownership**: `node_add_arg` deep-copies the `Word` (segment array),
  because the lexer token is reused for the next token. Shallow copies turn
  every argument into a duplicate of the last token.
- **fd save/restore**: redirections run in-process for builtins and compound
  commands via `save_fd`/`restore_redirs` plus a `dup(0/1/2)`+`dup2` bracket.
  Any new execution path that applies redirs must restore, otherwise fd 0/1/2
  leak into subsequent commands. `apply_redirs` sets the module-level
  `saved_fds` table, so nested save/restore is not supported; do not nest.
- **Flow control**: `g_flow` (BREAK/CONTINUE/RETURN) is set by builtins and
  consumed by the loop executors. Reset to `FLOW_NONE` after handling.
- **`$((` vs `$(`**: arithmetic must be detected before command substitution
  in `expand_into`, otherwise the body is run as a subshell.
- **`read` builtin uses `read(2)`**, not `getchar()`. stdio buffering hides
  input from `dup2`'d heredocs and pipes.
- **Heredoc bodies** are consumed by the lexer at `<<` time and stored on the
  token (`tok->heredoc`); the parser moves them onto the `Redir`. The parser
  must never see the body as ordinary words.

## Conventions

- C11 (`-std=gnu11`), compiled with `-Wall -Wextra`. Keep warnings clean;
  `-Wno-unused-result` is already enabled for `write()`/`read()`.
- Every module includes `osh.h` first. Shared declarations live there; static
  helpers stay in their `.c` file.
- Manual memory management via `xmalloc`/`xrealloc`/`free`; growable buffers
  via `Str`/`Vec`; hash map via `Map` (FNV-1a). No Boehm, no arenas.
- Public function names are prefixed by module (`var_`, `lex_`, `exec_`,
  `node_`, `str_`, `vec_`, `map_`) or are the documented shell API.
- Simplifications that are deliberately limited are marked with a
  `ponytail:` comment naming the ceiling and the upgrade path.

## Testing policy

- `--self-test` covers arithmetic, variables, globbing, control flow,
  builtins, and parameter expansion. Extend `test_*` functions in `osh.c`;
  use `check_int`/`check_str`/`check_status`.
- For parser/lexer fixes, prefer a self-test over a one-off script.
- The interactive line editor cannot be exercised without a tty; verify it
  manually with `script -qec "echo hi | ./osh -i" /dev/null`.

## Things that are intentionally simple (ponytail list)

- `saved_fds` is a fixed 32-entry table; deeply nested redirections fail.
- `match_prefix` is O(n^2) worst case; fine for interactive substitution.
- Signal handling traps bodies run synchronously via `run_string`, not
  asynchronously from the signal handler.
- No line editing beyond the raw termios loop; no readline, no multibyte
  awareness.
- Globbing does not sort or support `extglob`.
- Sessions are a plain byte stream: remote clients get the terminal driver's
  line editing, not the raw-mode editor, and Ctrl-C cannot interrupt a running
  command (upgrade path: a pty pair per client).
- Completion matches by prefix only: no glob-aware completion, and `$VAR`
  candidates come from the global and function scopes.

## Do not

- Do not add a dependency, a build system other than the Makefile, or a test
  framework. The self-test is the harness.
- Do not change the shell name or the public `osh`/`Oricade Shell` identity.
- Do not introduce interactive-only behavior into the non-interactive path;
  `g_interactive` gates prompt and history code.
