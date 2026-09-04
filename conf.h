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

#ifndef CRUFT_NO_GLOBALS
const char *term_name = "cruft-256color";

/* Linux framebuffer console only */
const char *fb_path = "/dev/fb0";
const char *shell_cmd = "/bin/bash";
#else
extern const char *term_name;
extern const char *fb_path;
extern const char *shell_cmd;
#endif

#endif /* CRUFT_CONF_H */
