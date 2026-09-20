# osh (Oricade Shell)

osh adalah shell Unix ringan (lightweight) yang ditulis dalam C murni (C11), dirancang sebagai alternatif minimalis namun fungsional untuk bash dan zsh.

## Fitur Utama

- **Lexer & Parser Sadar Konteks**: Mendukung quote tunggal/ganda, escaping `\`, operator logika (`&&`, `||`), pipe (`|`), rantai perintah (`;`), dan backgrounding (`&`).
- **Ekspansi Parameter & Variabel**:
  - `$VAR`, `${VAR}`, `${VAR:-default}`, `${VAR:+alternate}`, `${VAR:?err}`, `${VAR:=val}`
  - Substring: `${VAR:offset}`, `${VAR:offset:length}`, `${VAR: -n}`
  - Substitusi: `${VAR/pat/repl}`, `${VAR//pat/repl}` (mendukung glob pattern)
  - Panjang: `${#VAR}`
  - Variabel khusus: `$?`, `$$`, `$#`, `$!`, `$*`, `$@`, `$0`, `$-`
  - Command substitution: `$(cmd)` dan `` `cmd` ``
  - Aritmatika terintegrasi: `$(( expr ))` mendukung operasi matematika dasar dan dereferensi variabel/argumen `$1`.
- **Pengalihan I/O & Redirection**:
  - Input (`<`), Output (`>`), Append (`>>`), Clobber (`>|`)
  - File descriptor redirection (`2>&1`, `2>file`, `1>&2`)
  - Here-documents (`<<EOF`, `<<-EOF`) dan Here-strings (`<<<`)
  - Redirection menempel pada compound command: `while ...; done < file`
- **Struktur Kontrol Alur**:
  - `if ... then ... elif ... else ... fi`
  - `for var in ...; do ...; done`
  - `while ...; do ...; done` / `until ...; do ...; done`
  - `case ... in ... ;; ... esac`
  - Subshell `( ... )` dan Command Group `{ ...; }`
- **Fungsi & Alias**:
  - Pendefinisian fungsi: `name() { ... }` dengan argumen posisi (`$1`, `$2`, dll.)
  - Perintah `alias` dan `unalias`
- **Line Editor Interaktif & History**:
  - Dukungan navigasi panah kiri/kanan, Home/End, Backspace, Ctrl+A/E/U/K/L
  - Riwayat perintah tersimpan otomatis ke `~/.osh_history`
  - Prompt kustom via `$PS1` (mendukung escape sequence seperti `\u`, `\h`, `\w`, `\$`)
  - Tab completion untuk executable file dan nama path lokal
- **Builtin Shell (~40 perintah)**:
  - `cd`, `pwd`, `echo` (opsi `-n`, `-e`), `printf`, `export`, `unset`
  - `type`, `source` / `.`, `test` / `[`, `read`, `eval`, `exit`, dll.
- **Job Control & Sinyal**:
  - Background job dengan `&`, `jobs`, `wait`, `kill`
  - `trap` untuk `EXIT`, `INT`, `ERR`, `TERM`, `HUP`

## Kompilasi & Menjalankan

```bash
make
./osh
```

Menjalankan test suite bawaan:

```bash
./osh --self-test
# atau
make check
```

Menjalankan satu perintah langsung:

```bash
./osh -c 'echo "Tahun saat ini: $(date +%Y)"'
```
