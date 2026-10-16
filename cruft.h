/* See LICENSE for licence details. */
#define _XOPEN_SOURCE 600
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#include "glyph.h"
#include "color.h"

enum char_code {
	/* 7 bit */
	BEL = 0x07, BS  = 0x08, HT  = 0x09,
	LF  = 0x0A, VT  = 0x0B, FF  = 0x0C,
	CR  = 0x0D, ESC = 0x1B, DEL = 0x7F,
	/* others */
	SPACE     = 0x20,
	BACKSLASH = 0x5C,
};

enum misc {
	BUFSIZE            = 1024,             /* read, esc, various buffer size */
	BITS_PER_BYTE      = 8,                /* bits per byte */
	BYTES_PER_PIXEL    = sizeof(uint32_t), /* pixel size of sixel pixmap data */
	BITS_PER_SIXEL     = 6,                /* number of bits of a sixel */
	ESCSEQ_SIZE        = 1024,             /* limit size of terminal escape sequence */
	SELECT_TIMEOUT     = 15000,            /* used by select() */
	SLEEP_TIME         = 30000,            /* sleep time at EAGAIN, EWOULDBLOCK (usec) */
	MAX_ARGS           = 16,               /* max parameters of csi/osc sequence */
	UCS2_CHARS         = 0x10000,          /* number of UCS2 glyphs */
	CTRL_CHARS         = 0x20,             /* number of ctrl_func */
	ESC_CHARS          = 0x80,             /* number of esc_func */
	DRCS_CHARSETS      = 63,               /* number of charset of DRCS (according to DRCSMMv1) */
	GLYPHS_PER_CHARSET = 96,               /* number of glyph of each DRCS charset */
	DRCS_CHARS         = DRCS_CHARSETS * GLYPHS_PER_CHARSET,
	DEFAULT_CHAR       = SPACE,            /* used for erase char */
	BRIGHT_INC         = 8,                /* value used for brightening color */
	SBDISK_CACHE_ROWS  = 8,                /* decoded disk rows held in RAM (sbdisk.c) */
	KITTY_DYN_FG       = 254,              /* palette slot shadowed by truecolor fg SGR */
	KITTY_DYN_BG       = 255,              /* palette slot shadowed by truecolor bg SGR */
	KITTY_PH_CODE      = 0x10EEEE,         /* kitty Unicode placeholder (private use) */
};

enum char_attr {
	ATTR_RESET     = 0,
	ATTR_BOLD      = 1, /* brighten foreground */
	ATTR_UNDERLINE = 4,
	ATTR_BLINK     = 5, /* brighten background */
	ATTR_REVERSE   = 7,
};

enum osc {
	OSC_GWREPT = 8900, /* OSC Ps: mode number of cruft GWREPT */
	OSC_KITTY_GRAPHICS = 1337, /* kitty graphics protocol (File;/Image;) */
};

enum term_mode {
	MODE_RESET   = 0x00,
	MODE_ORIGIN  = 0x01, /* origin mode: DECOM */
	MODE_CURSOR  = 0x02, /* cursor visible: DECTCEM */
	MODE_AMRIGHT = 0x04, /* auto wrap: DECAWM */
	MODE_VWBS    = 0x08, /* variable-width backspace */
};

enum esc_state {
	STATE_RESET = 0x00,
	STATE_ESC   = 0x01, /* 0x1B, \033, ESC */
	STATE_CSI   = 0x02, /* ESC [ */
	STATE_OSC   = 0x04, /* ESC ] */
	STATE_DCS   = 0x08, /* ESC P */
	STATE_KPND  = 0x10, /* ESC _: awaiting G (kitty APC graphics) */
};

enum glyph_width {
	NEXT_TO_WIDE = 0,
	HALF,
	WIDE,
};

struct margin_t { uint16_t top, bottom; };
struct point_t { uint16_t x, y; };
struct color_pair_t { uint8_t fg, bg; };

struct cell_t {
	const struct glyph_t *glyphp;   /* pointer to glyph */
	struct color_pair_t color_pair; /* color (fg, bg) */
	enum char_attr attribute;       /* bold, underscore, etc... */
	enum glyph_width width;         /* wide char flag: WIDE, NEXT_TO_WIDE, HALF */
	bool has_pixmap;                /* has sixel pixmap data or not */
	uint8_t *pixmap;                /* NULL or BYTES_PER_PIXEL * CELL_WIDTH * CELL_HEIGHT heap block */
};

struct esc_t {
	char *buf;
	char *bp;
	int size;
	enum esc_state state;
	/* which string introduced the DCS body: 0 = ESC P (sixel et al),
	 * '+' = kitty graphics, '_' = kitty APC (_G) graphics */
	uint8_t dcs_kind;
};

struct charset_t {
	uint32_t code; /* UCS4 code point: cruft only prints UCS2 and DRCSMMv1 */
	int following_byte, count;
	bool is_valid;
};

struct state_t {   /* for save, restore state */
	struct point_t cursor;
	enum term_mode mode;
	enum char_attr attribute;
};

/* Cursor + scroll margins parked while the alternate screen (DECALT,
 * CSI ?1049h) is resident: the primary is never written to in the
 * meantime, so only these two escape a full-screen repaint. */
struct alt_saved_t {
	struct point_t cursor;
	struct margin_t scroll;
};

struct sixel_canvas_t {
	uint8_t *pixmap;
	struct point_t point;
	int width, height;
	int line_length;
	uint8_t color_index;
	uint32_t color_table[COLORS];
};

/* Per-screen state. scr[0] (primary) is allocated eagerly by term_init();
 * scr[1] (alternate) stays reserved until DECALT lands. Shared state
 * (SGR, charsets, palette, DRCS, sixel canvas) intentionally stays in
 * struct terminal_t: it is screen-independent by DEC definition. */
struct screen_t {
	struct cell_t **cells;        /* pointer to each line: cells[y] is line y */
	bool *line_dirty;             /* dirty flag */
	bool *tabstop;                /* tabstop flag */
	struct margin_t scroll;       /* scroll margin */
	struct point_t cursor;        /* cursor pos (x, y) */
	bool wrap_occurred;           /* whether auto wrap occurred or not */
	bool allocated;               /* cell matrix backing store exists */
};

/* Disk tier of the scrollback: session-scoped log file in /var/tmp
 * (mkstemp, 0600, unlinked on exit) holding rows that fell out of the
 * RAM ring. One variable-length record per row (packed cells + pixmap
 * table); a full-record-offset dir in RAM makes browsing O(1). When
 * the file reaches its cap the oldest quarter is compacted away.
 * Glyph pointers are not persistable: rows serialize as
 * (fg, bg, attr, width, bitmap) and decode through an intern table,
 * so cached rows share one glyph block per distinct (width, bitmap).
 * Any file damage (crash, metric mismatch) degrades the tier: RAM
 * keeps working, the file is dropped. Little-endian on disk. */
struct sbdisk_intern_t; /* (width, bitmap) key + owned glyph block */
struct sbdisk_t {
	int fd;                          /* -1 = tier not attached / degraded */
	char path[128];                  /* temp file, unlinked on detach */
	uint32_t cap;                    /* rows the file may hold */
	uint32_t count;                  /* rows held */
	uint64_t file_pos;               /* end of the newest record */
	uint64_t *off;                   /* record start offset, oldest first */
	size_t off_alloc;
	uint64_t evicted;                /* records dropped from the head */
	uint64_t epoch;                  /* bumped on compaction: ages cache */
	struct {
		uint64_t gen, epoch;
		struct cell_t *cells;        /* NULL = free slot */
	} cache[SBDISK_CACHE_ROWS];
	struct sbdisk_intern_t *glyphs;  /* (width, bitmap) -> glyph block */
	size_t glyph_n, glyph_alloc;
	unsigned cache_tick;
};

/* Scrollback: two tiers over the rows drained from the primary while a
 * full-screen scroll-up erases them (sub-regions and the alt screen
 * never feed it). The RAM ring holds the newest cap rows; when the
 * requested total depth (CRUFT_HISTORY) exceeds it, the older rows
 * spill to the disk tier (cap + disk.cap = requested total). Both own
 * their cell arrays: pixmap blocks are duplicated on intake and cleared
 * on eviction, so live-screen destruction can never alias stored
 * history. head = next write slot (= oldest slot when full).
 * view = rows the renderer shows above the live screen (0 = live tail);
 * while it points at stored rows a new push increments it, keeping the
 * browsed row in place. */
struct scrollback_t {
	unsigned cap;              /* RAM ring depth (0 = history disabled) */
	unsigned count;            /* rows in the RAM ring */
	unsigned head;             /* next write slot */
	size_t pixmap_bytes;       /* sum of sixel payloads held by the ring */
	struct cell_t **rows;      /* ring slots, one heap row array each */
	struct sbdisk_t disk;      /* older rows (empty when total <= cap) */
	unsigned view;             /* rows shown above the live screen */
};

/* One parsed OSC 1337 argument list (kitty graphics protocol).
 * Pointer members borrow memory from the caller's escape buffer, valid
 * until reset_esc(). */
struct kitty_args {
	const char *action;      /* "File" or "Image" */
	const char *data;        /* payload (base64 or raw, see .base64) */
	size_t data_len;
	long width, height;      /* -1 = not given (mandatory for raw) */
	int preserve_aspect;    /* a=, default 1 */
	int base64;             /* b=, default 1 */
	int transparency;       /* t=, default 0 */
	const char *id;         /* i= transfer id (Image); length: id_len */
	size_t id_len;          /* 0 = absent */
	const char *method;     /* m= operation (Image); length: method_len */
	size_t method_len;      /* 0 = absent */
};

/* Kitty graphics (APC _G / DCS +) state.
 *
 * Transfers arrive as one or more "<ESC>_G<ctrl>;<b64><ESC>\" strings
 * (the spec's DCS form is written "<ESC>+"; both intros are accepted).
 * The control data is a comma-separated key=value list; the payload after
 * the first ';' is base64 pixel data (or a PNG for f=100). */
struct kitty_xfer {
	uint8_t *raw;           /* growing pixel buffer */
	size_t len, cap;
	unsigned w, h, chan;   /* from the header chunk (s=/v=/f=) */
	unsigned first_red;    /* f=24/32/34 start with R, f=33/35 with B */
	int fmt;                /* requested f= */
	/* header-chunk parameters, kept for the m=0 completion chunk,
	 * which carries none of its own */
	uint32_t id;
	unsigned has_id, U, q, a;
	unsigned c, r, has_c, has_r;
	bool have, bad;         /* header seen / transfer poisoned */
};

#define KITTY_STORE_SLOTS 4

/* A finished transfer kept for placement (a=p or Unicode placeholders). */
struct kitty_store {
	bool used;
	uint32_t id;
	uint32_t *img;          /* 0x00RRGGBB, w*h */
	unsigned w, h;
	unsigned vc, vr;        /* virtual placement rect in cells */
};

/* Receiver state for the kitty Unicode placeholder display mechanism:
 * the client emits U+10EEEE cells whose SGR foreground encodes the image
 * id (truecolor rgb = low 24 bits) followed by up to three diacritics
 * encoding row, column and the high id byte. */
struct kitty_ph {
	bool pend;              /* placeholder seen, collecting diacritics */
	int nd;
	unsigned d[3];
	unsigned ax, ay;        /* cell the placeholder occupied */
	unsigned fg;            /* SGR fg at the placeholder cell */
	struct {
		bool on;
		uint32_t id;
		unsigned vc, vr;
		unsigned rows_done;
	} region;
};

struct terminal_t {
	int fd;                                  /* master of pseudo terminal */
	int width, height;                       /* terminal size (pixel) */
	uint16_t oy;                              /* top status strip height (px) */
	int cols, lines;                         /* terminal size (cell) */
	struct screen_t scr[2];                  /* scr[0]: primary, scr[1]: alternate (reserved) */
	unsigned active;                         /* index of the active screen in scr[] */
	struct scrollback_t sb;                  /* history ring (primary screen only) */
	enum term_mode mode;                     /* for set/reset mode */
	struct state_t state;                    /* for restore */
	struct alt_saved_t alt_saved;            /* cursor/margins parked in DECALT */
	struct color_pair_t color_pair;          /* color (fg, bg) */
	enum char_attr attribute;                /* bold, underscore, etc... */
	struct charset_t charset;                /* store UTF-8 byte stream */
	struct esc_t esc;                        /* store escape sequence */
	uint32_t virtual_palette[COLORS];        /* virtual color palette: always 32bpp */
	bool palette_modified;                   /* true if palette changed by OSC 4/104 */
	/* glyphs: demand-paged via glyph_get()/glyph_lookup() */
	struct glyph_t *drcs[DRCS_CHARSETS];     /* lazy DRCS charset tables */
	struct sixel_canvas_t sixel;
	/* kitty graphics */
	struct kitty_xfer kgx;
	struct kitty_store kgs[KITTY_STORE_SLOTS];
	struct kitty_ph kph;
};

/* The screen the cursor, cells and scroll margins currently belong to.
 * Centralizes active-screen selection so DECALT can toggle term->active
 * in one place without touching every cell/cursor access. */
static inline struct screen_t *active_screen(struct terminal_t *term)
{
	return &term->scr[term->active];
}

struct parm_t { /* for parse_arg() */
	int argc;
	char *argv[MAX_ARGS];
};

/* cell_pixmap.c */
size_t cell_pixmap_bytes(void);
void cell_pixmap_clear(struct cell_t *c);
uint8_t *cell_pixmap_ensure(struct cell_t *c);
int cell_pixmap_dup(struct cell_t *dst, const struct cell_t *src);

/* drcs.c */
struct glyph_t *drcs_ensure(struct terminal_t *term, int charset);
void drcs_clear_charset(struct terminal_t *term, int charset);
void drcs_clear_all(struct terminal_t *term);
const struct glyph_t *drcs_glyph(struct terminal_t *term, uint32_t code);
int drcs_clamp_char_index(int idx);

/* sixel_canvas.c */
int sixel_canvas_ensure(struct terminal_t *term);
void sixel_canvas_free(struct terminal_t *term);
void term_release_transient(struct terminal_t *term);
size_t sixel_cell_row_bytes(int cell_x, int term_width);
void reset_sixel(struct terminal_t *term, struct color_pair_t color_pair, int width, int height);
void sixel_copy2cell(struct terminal_t *term, struct sixel_canvas_t *sc);

/* kitty_image.c */
int kitty_base64_decode(const char *in, size_t len, uint8_t *out, size_t cap, size_t *out_len);
int kitty_args_parse(const char *payload, struct kitty_args *ka);
bool kitty_png_available(void);
void kitty_image_dispatch(struct terminal_t *term, const char *payload);
void kitty_dcs_dispatch(struct terminal_t *term, const char *body);
bool kitty_ph_feed(struct terminal_t *term, uint32_t code);
void kitty_ph_newline(struct terminal_t *term);
void kitty_ph_invalidate(struct terminal_t *term);
void kitty_xfer_abort(struct terminal_t *term);
void kitty_grab_reset(struct terminal_t *term);
void kitty_grab_free(struct terminal_t *term);

/* terminal cells (terminal_cell.c) */
void erase_cell(struct terminal_t *term, int y, int x);
void copy_cell(struct terminal_t *term, int dst_y, int dst_x, int src_y, int src_x);

/* scrollback (scrollback.c + sbdisk.c) */
void sb_init(struct terminal_t *term);
void sb_die(struct terminal_t *term);
void sb_reset(struct terminal_t *term);
void sb_feed_line(struct terminal_t *term, const struct cell_t *row);
const struct cell_t *sb_view_row(struct terminal_t *term, int line);
int sb_consume_nav(struct terminal_t *term, const uint8_t *buf, int n,
		   unsigned *keep);
/* Most bytes sb_consume_nav can hand back for the next read: the longest
 * nav sequence (Ctrl+Home/End, 6 bytes) minus one. */
#define SB_NAV_HOLD_MAX 5
void sb_scrub_freed_glyphs(const struct terminal_t *term, const struct glyph_t *freed, int n);
unsigned sb_history_total(const struct terminal_t *term); /* RAM + disk rows */

/* sbdisk (sbdisk.c) */
void sbdisk_attach(struct terminal_t *term);       /* opens temp file (lazy) */
void sbdisk_detach(struct terminal_t *term);       /* closes + unlinks, frees */
int sbdisk_store(struct terminal_t *term, const struct cell_t *row);
const struct cell_t *sbdisk_fetch(struct terminal_t *term, unsigned k_from_tail);

#ifndef CRUFT_NO_GLOBALS
const uint8_t attr_mask[] = {
	0x00, 0x01, 0x00, 0x00, /* 0:none      1:bold  2:none 3:none */
	0x02, 0x04, 0x00, 0x08, /* 4:underline 5:blink 6:none 7:reverse */
};

const uint32_t bit_mask[] = {
	0x00,
	0x01,       0x03,       0x07,       0x0F,
	0x1F,       0x3F,       0x7F,       0xFF,
	0x1FF,      0x3FF,      0x7FF,      0xFFF,
	0x1FFF,     0x3FFF,     0x7FFF,     0xFFFF,
	0x1FFFF,    0x3FFFF,    0x7FFFF,    0xFFFFF,
	0x1FFFFF,   0x3FFFFF,   0x7FFFFF,   0xFFFFFF,
	0x1FFFFFF,  0x3FFFFFF,  0x7FFFFFF,  0xFFFFFFF,
	0x1FFFFFFF, 0x3FFFFFFF, 0x7FFFFFFF, 0xFFFFFFFF,
};

volatile sig_atomic_t vt_active   = true;  /* SIGUSR1: vt is active or not */
volatile sig_atomic_t need_redraw = false; /* SIGUSR1: vt activated */
volatile sig_atomic_t child_alive = false; /* SIGCHLD: child process (shell) is alive or not */
struct termios termios_orig;
#else
extern const uint8_t attr_mask[];
extern const uint32_t bit_mask[];
extern volatile sig_atomic_t vt_active;
extern volatile sig_atomic_t need_redraw;
extern volatile sig_atomic_t child_alive;
extern struct termios termios_orig;
#endif
