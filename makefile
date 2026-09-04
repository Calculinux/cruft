CC ?= gcc

WARNFLAGS = -std=c99 -pedantic -Wall -Wextra -Werror
CFLAGS  ?= $(WARNFLAGS) -O3 -s -pipe
LDFLAGS ?=
ASAN_CFLAGS = $(WARNFLAGS) -O1 -g -fsanitize=address -fno-omit-frame-pointer
ASAN_LDFLAGS = -fsanitize=address

SRCS = cruft.c glyph_mmap.c cell_pixmap.c drcs.c sixel_canvas.c terminal_cell.c
# Standalone translation units tidy/cppcheck can analyze without fb/VT headers.
LINT_SRCS = glyph_mmap.c cell_pixmap.c drcs.c sixel_canvas.c terminal_cell.c tools/mkcruftfont.c
HDR = glyph.h cruft.h conf.h color.h parse.h terminal.h util.h \
	ctrlseq/esc.h ctrlseq/csi.h ctrlseq/osc.h ctrlseq/dcs.h \
	fb/common.h fb/linux.h

DESTDIR   =
PREFIX    = $(DESTDIR)/usr
MANPREFIX = $(DESTDIR)/usr/share/man

TEST_SRCS = tests/selftest.c cell_pixmap.c drcs.c sixel_canvas.c glyph_mmap.c terminal_cell.c
TEST_CFLAGS = $(WARNFLAGS) -O0 -g -I.

CLANG_TIDY ?= clang-tidy
CPPCHECK ?= cppcheck
SHELLCHECK ?= shellcheck

.PHONY: all clean install uninstall test test-asan lint tidy cppcheck shellcheck check

all: cruft mkcruftfont

cruft: $(SRCS) $(HDR)
	# Glyphs: mmap .cruftfont blob (glyph_mmap.c); runtime cell metrics from blob header.
	$(CC) -o $@ $(SRCS) $(CFLAGS) $(LDFLAGS)

mkcruftfont: tools/mkcruftfont.c
	$(CC) -o $@ $< $(CFLAGS) $(LDFLAGS)

test: cruft mkcruftfont tests/selftest
	./tests/selftest
	./mkcruftfont --self-check

tests/selftest: $(TEST_SRCS) $(HDR)
	$(CC) -o $@ $(TEST_SRCS) $(TEST_CFLAGS) $(LDFLAGS)

test-asan: $(SRCS) $(HDR) tests/selftest.c
	$(CC) -o tests/selftest-asan $(TEST_SRCS) $(ASAN_CFLAGS) -I. $(ASAN_LDFLAGS)
	./tests/selftest-asan

# Compiler-as-linter: catch warnings as errors without linking.
lint:
	$(CC) -fsyntax-only $(WARNFLAGS) -O2 -I. $(SRCS)
	$(CC) -fsyntax-only $(WARNFLAGS) -O2 tools/mkcruftfont.c
	$(CC) -fsyntax-only $(WARNFLAGS) -O0 -g -I. $(TEST_SRCS)

tidy:
	$(CLANG_TIDY) $(LINT_SRCS) -- $(WARNFLAGS) -O2 -I.

cppcheck:
	$(CPPCHECK) --error-exitcode=1 --std=c99 --enable=warning,style,performance \
		--suppress=missingIncludeSystem --inline-suppr -I. $(LINT_SRCS)

shellcheck:
	$(SHELLCHECK) -x cruft_wall

check: lint tidy cppcheck shellcheck test

install: cruft mkcruftfont
	mkdir -p $(PREFIX)/share/terminfo
	tic -o $(PREFIX)/share/terminfo info/cruft.src
	mkdir -p $(PREFIX)/bin/
	install -m755 ./cruft $(PREFIX)/bin/cruft
	install -m755 ./cruft_wall $(PREFIX)/bin/cruft_wall
	install -m755 ./mkcruftfont $(PREFIX)/bin/mkcruftfont
	mkdir -p $(MANPREFIX)/man1/
	install -m644 ./man/cruft.1 $(MANPREFIX)/man1/cruft.1
	mkdir -p $(PREFIX)/share/cruft
	@echo "note: place a .cruftfont in $(PREFIX)/share/cruft/ (console-font package)"

uninstall:
	rm -f $(PREFIX)/bin/cruft
	rm -f $(PREFIX)/bin/cruft_wall
	rm -f $(PREFIX)/bin/mkcruftfont
	rm -f $(MANPREFIX)/man1/cruft.1

clean:
	rm -f cruft mkcruftfont tests/selftest tests/selftest-asan
