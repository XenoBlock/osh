# osh (Oricade Shell)

A lightweight Unix shell written in pure C11, designed as a minimalist but
functional alternative to bash and zsh. No dependencies beyond the C standard
library and POSIX.

## Features

- **Context-aware lexer & parser**: single/double quotes, `\` escaping, logical
  operators (`&&`, `||`), pipes (`|`), command lists (`;`), backgrounding (`&`)
- **Parameter & variable expansion**:
  - `$VAR`, `${VAR}`, `${VAR:-default}`, `${VAR:+alt}`, `${VAR:?err}`, `${VAR:=val}`
  - Substring: `${VAR:offset}`, `${VAR:offset:length}`, `${VAR: -n}`
  - Substitution: `${VAR/pat/repl}`, `${VAR//pat/repl}` (glob patterns supported)
  - Length: `${#VAR}`
  - Special variables: `$?`, `$$`, `$#`, `$!`, `$*`, `$@`, `$0`, `$-`
  - Command substitution: `$(cmd)` and `` `cmd` ``
  - Integrated arithmetic: `$(( expr ))` with the usual operators and variable
    dereference
- **I/O redirection**:
  - Input (`<`), output (`>`), append (`>>`), clobber (`>|`)
  - File descriptor redirection (`2>&1`, `2>file`, `1>&2`)
  - Here-documents (`<<EOF`, `<<-EOF`) and here-strings (`<<<`)
  - Redirections attached to compound commands: `while ...; done < file`
- **Control flow**:
  - `if ... then ... elif ... else ... fi`
  - `for var in ...; do ...; done`
  - `while ...; do ...; done` / `until ...; do ...; done`
  - `case ... in ... ;; ... esac`
  - Subshell `( ... )` and command group `{ ...; }`
  - `break`, `continue`, `return`
- **Functions & aliases**: `name() { ... }` with positional parameters,
  `alias`/`unalias`
- **Interactive line editor & history**:
  - Arrow keys, Home/End, Backspace, Ctrl+A/E/U/K/L
  - History persisted to `~/.osh_history`
  - Custom prompt via `$PS1` (escapes `\u`, `\h`, `\w`, `\$`)
  - Tab completion for executables and path names
- **~40 shell builtins**: `cd`, `pwd`, `echo`, `printf`, `export`, `unset`,
  `type`, `source`/`.`, `test`/`[`, `read`, `eval`, `exit`, and more
- **Job control & signals**: background jobs with `&`, `jobs`, `wait`, `kill`,
  and `trap` for `EXIT`, `INT`, `ERR`, `TERM`, `HUP`
- **Globbing**: `*`, `?`, `[...]` with recursive directory expansion

## Build & run

```bash
make
./osh
```

Run the built-in test suite:

```bash
./osh --self-test
# or
make check
```

Run a single command:

```bash
./osh -c 'echo "Year: $(date +%Y)"'
```

Run a script file:

```bash
./osh script.sh
```

Interactive mode with login rc file (`~/.oshrc`):

```bash
./osh -il
```

### Command line options

| Option | Meaning                                        |
|--------|------------------------------------------------|
| `-c`   | read commands from the following string        |
| `-i`   | force interactive mode                          |
| `-l`   | login shell: read `~/.oshrc`                    |
| `-s`   | read commands from stdin (default if no tty)    |
| `-f`   | disable filename globbing                       |
| `-e`   | exit on the first failed command                |
| `-u`   | treat unset variables as errors                 |
| `-x`   | print commands as they run (xtrace)             |
| `-v`   | verbose: print input lines                      |

## Project layout

| File          | Responsibility                                              |
|---------------|-------------------------------------------------------------|
| `osh.h`       | shared declarations: lexer, AST, job table, map/str/vec     |
| `osh.c`       | entry point, option parsing, self-test suite                |
| `input.c`     | raw line reading for the `read` builtin                     |
| `lex.c`       | tokenizer, quote handling, here-document reading            |
| `parse.c`     | recursive-descent parser producing the AST                  |
| `var.c`       | variables, expansion, word splitting, globbing, arithmetic  |
| `exec.c`      | pipelines, redirection, job control, compound commands      |
| `builtin.c`   | the built-in command table                                   |
| `edit.c`      | interactive line editor, history, tab completion            |
| `jobs.c`      | job table and status reporting                               |
| `match.c`     | shell pattern matching and `[[ ]]` conditionals             |
| `util.c`      | allocator, growable string/vector, hash map, path helpers   |
| `Makefile`    | build and `check` target                                     |

## License

MIT. See [LICENSE](LICENSE).
