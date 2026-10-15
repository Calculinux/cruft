/* conf.h: Calculinux/cruft defaults for PicoCalc / Luckfox Lyra */
/* Fonts via CRUFT_FONT: miniwi 4x8, console 6x12 (default), unifont 8x16. */
#ifndef CRUFT_CONF_H
#define CRUFT_CONF_H

enum {
	DEFAULT_FG           = 7,
	DEFAULT_BG           = 0,
	ACTIVE_CURSOR_COLOR  = 2,
	PASSIVE_CURSOR_COLOR = 1,
};

enum {
	VERBOSE          = false,
	TABSTOP          = 8,
	LAZY_DRAW        = true,
	BACKGROUND_DRAW  = false,
	VT_CONTROL       = true,
	FORCE_TEXT_MODE  = false,
	SUBSTITUTE_HALF  = 0x0020,
	SUBSTITUTE_WIDE  = 0x3000,
	REPLACEMENT_CHAR = 0x003F,
};

/* Scrollback: total depth (CRUFT_HISTORY), RAM tier depth
 * (CRUFT_SB_RAM), and per-record sixel payload kept on disk. */
enum {
	SB_DEFAULT_DEPTH    = 96,      /* rows, unless CRUFT_HISTORY */
	SB_RAM_LINES        = 512,     /* rows kept in RAM (spills past) */
	SB_DEPTH_MAX        = 16384,   /* CRUFT_HISTORY ceiling */
	SB_RECORD_PIXMAP_KB = 32,      /* sixel bytes kept per disk row */
	SB_PIXMAP_CAP_KIB   = 64,      /* sixel payload the RAM ring retains */
};

#ifndef CRUFT_NO_GLOBALS
const char *term_name = "cruft-256color";

/* Linux framebuffer console only */
const char *fb_path = "/dev/fb0";
const char *shell_cmd = "/bin/bash";

/* Status bar: compile-time default segments; CRUFT_SB env (via
 * /etc/default/console) overrides per session. Names: time, wifi,
 * battery (also "all" / "none" / "0"). */
const char *sb_default_segments = "all";
#else
extern const char *term_name;
extern const char *fb_path;
extern const char *shell_cmd;
extern const char *sb_default_segments;
#endif

#endif /* CRUFT_CONF_H */
