/*
 * Write-boundary capture tee for the L2b live driver (tests/apptest).
 *
 * The app runs on a REAL pty: the pinned ratatui-image picker routes
 * its raw-mode negotiation through rustix raw syscalls, which no
 * LD_PRELOAD can interpose, so the terminal must be genuine. The
 * line discipline in that pty is empirically lossy in transit (see
 * REPORT.md), so the pty read side is NOT the authority for what the
 * app authored. This shim records every write() the app issues to
 * its stdout, byte for byte, at the moment of the call - before the
 * kernel touches it - and mirrors those bytes onto an inherited pipe
 * for the driver. The real write then proceeds untouched: the pty
 * remains a delivery-only duct (it carries the product's replies to
 * the app, and the app's outgoing bytes onward, where the driver
 * discards them).
 *
 * Coverage note: only write() is hooked. The app's entire protocol
 * output (picker probe, crossterm backend flush, ratatui widget
 * draws) funnels through std::io::Stdout -> the libc write symbol;
 * nothing reaches the pty another way. Should that ever change
 * (writev, splice, a direct svc), the zero-budget payload assertion
 * in apptest fails loudly instead of capturing silently.
 *
 * Ordering/locking: the app serializes all stdout traffic through
 * std's Stdout lock, so mirror calls are already ordered and none is
 * concurrent; no lock is taken.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* fixed capture fd; must agree with the driver (apptest.c). The
 * driver dup2's the pipe's write end onto it in the child, and no
 * other fd the app opens can land here while the tee holds it. */
#define TEE_FD 7

typedef ssize_t (*write_fn)(int, const void *, size_t);

static write_fn g_real;
static struct stat g_out_id;
static int g_have_id;
static FILE *g_log;	/* CRUFT_TEE_LOG, diagnostic only */

__attribute__((constructor))
static void tee_load(void)
{
	const char *lp = getenv("CRUFT_TEE_LOG");

	g_real = (write_fn) dlsym(RTLD_NEXT, "write");
	/* the app may also reach the pty through an aliased fd
	 * (e.g. /dev/tty): recognise stdout by file identity, not
	 * by number alone */
	g_have_id = (fstat(1, &g_out_id) == 0);
	if (lp && *lp)
		g_log = fopen(lp, "w");
	signal(SIGPIPE, SIG_IGN);	/* losing the driver must not
					 * kill the app with a bogus
					 * status */
}

static int is_app_stdout(int fd)
{
	struct stat st;

	if (fd == 1)
		return 1;
	if (!g_have_id)
		return 0;
	if (fstat(fd, &st) != 0)
		return 0;
	return st.st_dev == g_out_id.st_dev &&
	       st.st_ino == g_out_id.st_ino;
}

static void mirror(const void *buf, size_t n)
{
	while (n) {
		ssize_t w = g_real ? g_real(TEE_FD, buf, n): -1;

		if (w <= 0)
			return;		/* best effort: the driver is
					 * gone, the run is over */
		buf = (const uint8_t *) buf + (size_t) w;
		n -= (size_t) w;
	}
}

ssize_t write(int fd, const void *buf, size_t n)
{
	int app = is_app_stdout(fd);

	if (app)
		mirror(buf, n);		/* record the intent first */
	if (g_log) {
		/* diagnostic trace: which fd, how much, how many
		 * zeros, first 4 bytes - per call */
		const uint8_t *pb = buf ? (const uint8_t *) buf: NULL;
		size_t zeros = 0, i;

		for (i = 0; pb && i < n; i++)
			if (pb[i] == 0)
				zeros++;
		fprintf(g_log, "%s fd=%d len=%zu zeros=%zu hdr=%02x "
		        "%02x %02x %02x\n",
		        app? "APP ": "oth ", fd, n, zeros,
		        pb && n >= 1? pb[0]: 0,
		        pb && n >= 2? pb[1]: 0,
		        pb && n >= 3? pb[2]: 0,
		        pb && n >= 4? pb[3]: 0);
		fflush(g_log);
	}
	return g_real ? g_real(fd, buf, n): (errno = ENOSYS, -1);
}
