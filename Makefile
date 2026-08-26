# Building the standalone program needs nothing but a C compiler:
#
#     make            build ./cserpent
#     make test       run the regression tests
#     make install    copy ./cserpent into PREFIX/bin (default /usr/local)
#
# The pip package is a separate path; see README.md. Use it if you want
# 'import cserpent' and the notebook workflow as well as the command.

CC      ?= cc
CFLAGS  ?= -O2 -g
PREFIX  ?= /usr/local
PYTHON  ?= python3

cserpent: cserpent.c stb_c_lexer.h
	$(CC) $(CFLAGS) -o $@ cserpent.c

.PHONY: test
test: cserpent
	$(PYTHON) tests/run_tests.py

.PHONY: install
install: cserpent
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 cserpent $(DESTDIR)$(PREFIX)/bin/cserpent

.PHONY: clean
clean:
	rm -f cserpent _cserpent*.so
	rm -rf build dist *.egg-info
