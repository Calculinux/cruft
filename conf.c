/* Persistent defaults from /etc/cruft.conf (see conf_load below). */
#define CRUFT_NO_GLOBALS
#include "cruft.h"
#include "conf.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* /etc/cruft.conf: persistent defaults for the CRUFT_* environment
 * knobs, one KEY=VALUE per line ('#' comments, blanks ignored,
 * whitespace around the '=' allowed). The valid keys are exactly the
 * names the consumers already read from the environment; a file value
 * is installed only when the environment does not carry the key yet,
 * so per-service Environment= lines or an exported shell variable
 * keep precedence. Unknown keys and malformed values are warned about
 * and skipped - a bad config line must never take the console down. */
#define CONF_LINE_MAX 256

static void conf_warn(char *fmt, ...)
{
	va_list ap;

	/* The analyzer's valist check misfires on this straight-line
	 * va_start/vfprintf pair (clang 18.1; the identical body in
	 * util.h's logging passes the same lane) - suppress the false
	 * positive rather than bend correct code around it. */
	fputs(">>WARN<<\t", stderr);
	va_start(ap, fmt);
	/* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) */
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static bool conf_is_digits(const char *s)
{
	if (*s == '\0')
		return false;
	for (; *s; s++)
		if (!isdigit((unsigned char) *s))
			return false;
	return true;
}

static const char *conf_keys[] = {
	"CRUFT_FONT",
	"CRUFT_SB",
	"CRUFT_SB_FONT",
	"CRUFT_SB_12H",
	"CRUFT_HISTORY",
	"CRUFT_SB_RAM",
};

static bool conf_value_ok(const char *key, const char *val)
{
	size_t i, n;

	if (*val == '\0')
		return false;
	n = sizeof(conf_keys) / sizeof(conf_keys[0]);
	for (i = 0; i < n; i++) {
		if (strcmp(key, conf_keys[i]) != 0)
			continue;
		if (strcmp(key, "CRUFT_HISTORY") == 0
		    || strcmp(key, "CRUFT_SB_RAM") == 0)
			return conf_is_digits(val);
		if (strcmp(key, "CRUFT_SB_12H") == 0)
			return strcmp(val, "0") == 0 || strcmp(val, "1") == 0;
		if (strcmp(key, "CRUFT_SB") == 0) {
			for (const char *p = val; *p; p++)
				if (!isalnum((unsigned char) *p)
				    && *p != ',' && *p != '_')
					return false;
			return true;
		}
		return true; /* CRUFT_FONT / CRUFT_SB_FONT: opaque path */
	}
	return false; /* not a known key */
}

void conf_load_file(const char *path)
{
	FILE *fp;
	char line[CONF_LINE_MAX];
	char vals[sizeof(conf_keys) / sizeof(conf_keys[0])]
		  [CONF_LINE_MAX] = { { 0 } };
	size_t n = sizeof(conf_keys) / sizeof(conf_keys[0]);
	size_t i;

	fp = fopen(path, "r");
	if (fp == NULL)
		return; /* no config: environment and built-ins stand */
	/* The analyzer's unix.Stream checks misfire on the overlong-line
	 * drain (below): it assumes the drain runs to EOF, which would
	 * make this fgets a no-op on an "indeterminate" position. The
	 * drain stops at the next newline in every real case, so the
	 * loop keeps reading the rest of the file normally. */
	/* NOLINTNEXTLINE(clang-analyzer-unix.Stream) */
	while (fgets(line, sizeof(line), fp)) {
		char *key = line;
		char *eq;
		char *val;
		char *endp;
		size_t len;

		if (strchr(line, '\n') == NULL && !feof(fp)) {
			int c;

			conf_warn("cruft.conf: line too long: %.48s...\n", line);
			/* Drain the tail of the overlong line: the rest
			 * would otherwise surface at the next fgets as if
			 * it were a new line, and a KEY=VALUE wrapped
			 * past the 255-byte boundary would install. */
			while ((c = fgetc(fp)) != EOF && c != '\n')
				;
			continue;
		}
		while (isspace((unsigned char) *key))
			key++;
		if (*key == '\0' || *key == '#')
			continue;
		eq = strchr(key, '=');
		if (eq == NULL) {
			conf_warn("cruft.conf: skipping line without '=': %.48s\n",
				  line);
			continue;
		}
		*eq = '\0';
		val = eq + 1;
		len = strlen(key);
		while (len > 0 && isspace((unsigned char) key[len - 1]))
			key[--len] = '\0';
		while (isspace((unsigned char) *val))
			val++;
		endp = val + strlen(val);
		while (endp > val && isspace((unsigned char) endp[-1]))
			*--endp = '\0';
		if (strlen(val) >= CONF_LINE_MAX) {
			conf_warn("cruft.conf: ignoring %s (value too long)\n", key);
			continue;
		}
		if (!conf_value_ok(key, val)) {
			conf_warn("cruft.conf: ignoring %s='%s' (unknown key or "
				  "malformed value)\n", key, val);
			continue;
		}
		for (i = 0; i < n; i++)
			if (strcmp(key, conf_keys[i]) == 0)
				break;
		/* Last valid line for a key wins (operator edits win);
		 * val is bounds-checked above. */
		snprintf(vals[i], CONF_LINE_MAX, "%s", val);
	}
	fclose(fp);
	for (i = 0; i < n; i++) {
		/* Environment precedence: a process-exported value wins;
		 * duplicates from the file are settled above. */
		if (vals[i][0] != '\0' && getenv(conf_keys[i]) == NULL)
			if (setenv(conf_keys[i], vals[i], 0) != 0)
				conf_warn("cruft.conf: setenv(%s) failed\n",
					  conf_keys[i]);
	}
}

void conf_load(void)
{
	conf_load_file(CONFIG_FILE);
}
