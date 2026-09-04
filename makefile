CC ?= gcc

CFLAGS  ?= -std=c99 -pedantic -Wall -Wextra -O3 -s -pipe
LDFLAGS ?=
ASAN_CFLAGS = -std=c99 -pedantic -Wall -Wextra -O1 -g -fsanitize=address -fno-omit-frame-pointer
ASAN_LDFLAGS = -fsanitize=address

SRCS = cruft.c glyph_mmap.c cell_pixmap.c drcs.c sixel_canvas.c
HDR = glyph.h cruft.h conf.h color.h parse.h terminal.h util.h \
	ctrlseq/esc.h ctrlseq/csi.h ctrlseq/osc.h ctrlseq/dcs.h \
	fb/common.h fb/linux.h

DESTDIR   =
PREFIX    = $(DESTDIR)/usr
MANPREFIX = $(DESTDIR)/usr/share/man

TEST_SRCS = tests/selftest.c cell_pixmap.c drcs.c sixel_canvas.c glyph_mmap.c
TEST_CFLAGS = -std=c99 -pedantic -Wall -Wextra -O0 -g -I.

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

install: cruft
	mkdir -p $(PREFIX)/share/terminfo
	tic -o $(PREFIX)/share/terminfo info/cruft.src
	mkdir -p $(PREFIX)/bin/
	install -m755 ./cruft $(PREFIX)/bin/cruft
	install -m755 ./cruft_wall $(PREFIX)/bin/cruft_wall
	mkdir -p $(MANPREFIX)/man1/
	install -m644 ./man/cruft.1 $(MANPREFIX)/man1/cruft.1
	mkdir -p $(PREFIX)/share/cruft

uninstall:
	rm -f $(PREFIX)/bin/cruft
	rm -f $(PREFIX)/bin/cruft_wall
	rm -f $(MANPREFIX)/man1/cruft.1

clean:
	rm -f cruft mkcruftfont tests/selftest tests/selftest-asan
