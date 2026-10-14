CC ?= gcc

WARNFLAGS = -std=c99 -pedantic -Wall -Wextra -Werror
CFLAGS  ?= $(WARNFLAGS) -O3 -s -pipe
LDFLAGS ?=
ASAN_CFLAGS = $(WARNFLAGS) -O1 -g -fsanitize=address -fno-omit-frame-pointer
ASAN_LDFLAGS = -fsanitize=address

SRCS = cruft.c glyph_mmap.c cell_pixmap.c drcs.c sixel_canvas.c kitty_image.c terminal_cell.c status.c
# Standalone translation units tidy/cppcheck can analyze without fb/VT headers
# (status.c includes the fb type shapes, so it stays out of LINT_SRCS).
LINT_SRCS = glyph_mmap.c cell_pixmap.c drcs.c sixel_canvas.c kitty_image.c terminal_cell.c tools/mkcruftfont.c
HDR = glyph.h cruft.h conf.h color.h parse.h terminal.h util.h status.h \
	ctrlseq/esc.h ctrlseq/csi.h ctrlseq/osc.h ctrlseq/dcs.h \
	fb/common.h fb/linux.h fb/fb.h

DESTDIR   =
PREFIX    = $(DESTDIR)/usr
MANPREFIX = $(DESTDIR)/usr/share/man

TEST_SRCS = tests/selftest.c cell_pixmap.c drcs.c sixel_canvas.c kitty_image.c glyph_mmap.c terminal_cell.c status.c
TEST_CFLAGS = $(WARNFLAGS) -O0 -g -I.

# Optional system libpng: PNG payloads on the kitty graphics path (File;).
# Precedence: explicit HAVE_PNG=1/0 beats all; then a PNG_CFLAGS/PNG_LIBS
# hint (cross builds: the OE recipe forwards pkg-config output); otherwise
# probe the toolchain. Builds without it compile and run, dropping PNG
# payloads with a note (raw BGR/BGRA and the Image/Rep path are unaffected).
ifeq ($(HAVE_PNG),)
ifneq ($(strip $(PNG_CFLAGS) $(PNG_LIBS)),)
HAVE_PNG := 1
else
# Probe the high-level png_image API specifically (libpng >= 1.6): the code
# only uses those entry points, so a libpng lacking them must be rejected.
# The probe instantiates the png_image type (a typedef of an anonymous
# struct, NOT "struct png_image") so broken/reduced headers fail here.
HAVE_PNG := $(shell printf '%s\n' \
	'#include <png.h>' \
	'int main(void){' \
	'png_image pi;' \
	'pi.version = PNG_IMAGE_VERSION;' \
	'(void) pi;' \
	'return (png_image_begin_read_from_memory != 0)' \
	'    + (png_image_finish_read != 0)' \
	'    + (png_image_free != 0);' \
	'}' | \
	$(CC) -x c - -std=c99 -lpng -o /dev/null 2>/dev/null && echo 1)
endif
endif
ifeq ($(HAVE_PNG),1)
KG_PNG_CFLAGS = -DHAVE_LIBPNG $(PNG_CFLAGS)
KG_PNG_LDFLAGS = $(or $(PNG_LIBS),-lpng)
CFLAGS += $(KG_PNG_CFLAGS)
TEST_CFLAGS += $(KG_PNG_CFLAGS)
endif

CLANG_TIDY ?= clang-tidy
CPPCHECK ?= cppcheck
SHELLCHECK ?= shellcheck

.PHONY: all clean install uninstall test test-asan lint tidy cppcheck shellcheck check

all: cruft mkcruftfont

cruft: $(SRCS) $(HDR)
	# Glyphs: mmap .cruftfont blob (glyph_mmap.c); runtime cell metrics from blob header.
	$(CC) -o $@ $(SRCS) $(CFLAGS) $(LDFLAGS) $(KG_PNG_LDFLAGS)

mkcruftfont: tools/mkcruftfont.c
	$(CC) -o $@ $< $(CFLAGS) $(LDFLAGS)

test: cruft mkcruftfont tests/selftest
	./tests/selftest
	./mkcruftfont --self-check

tests/selftest: $(TEST_SRCS) $(HDR)
	$(CC) -o $@ $(TEST_SRCS) $(TEST_CFLAGS) $(LDFLAGS) $(KG_PNG_LDFLAGS)

test-asan: $(SRCS) $(HDR) tests/selftest.c
	$(CC) -o tests/selftest-asan $(TEST_SRCS) $(ASAN_CFLAGS) $(KG_PNG_CFLAGS) -I. $(ASAN_LDFLAGS) $(KG_PNG_LDFLAGS)
	./tests/selftest-asan

# Compiler-as-linter: catch warnings as errors without linking.
lint:
	$(CC) -fsyntax-only $(WARNFLAGS) -O2 -I. $(KG_PNG_CFLAGS) $(SRCS)
	$(CC) -fsyntax-only $(WARNFLAGS) -O2 tools/mkcruftfont.c
	$(CC) -fsyntax-only $(WARNFLAGS) -O0 -g -I. $(KG_PNG_CFLAGS) $(TEST_SRCS)

tidy:
	$(CLANG_TIDY) $(LINT_SRCS) -- $(WARNFLAGS) -O2 -I. $(KG_PNG_CFLAGS)

cppcheck:
	$(CPPCHECK) --error-exitcode=1 --std=c99 --enable=warning,style,performance \
		--suppress=missingIncludeSystem --inline-suppr -I. $(KG_PNG_CFLAGS) $(LINT_SRCS)

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
