# osh - Oricade Shell
CC      ?= gcc
CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra -Wno-unused-result -g
LDFLAGS ?=
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

SRC = osh.c input.c util.c var.c lex.c parse.c exec.c builtin.c edit.c jobs.c match.c session.c
OBJ = $(SRC:.c=.o)

all: osh

osh: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c osh.h
	$(CC) $(CFLAGS) -c -o $@ $<

check: osh
	./osh --self-test

# Scratch files left behind by manual test runs (kept out of git).
SCRAP = c.txt dq fo.txt i.txt n0 n1 n2 n3 n4 o.txt r1 r2 r3 w.txt

clean:
	rm -f osh $(OBJ) $(SCRAP) core *.core

distclean: clean
	rm -f .osh_history

install: osh
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 osh $(DESTDIR)$(BINDIR)/osh

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/osh

.PHONY: all check clean distclean install uninstall
