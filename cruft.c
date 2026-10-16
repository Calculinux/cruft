/* See LICENSE for licence details. */
/* cruft.c: Calculinux reduced-footprint framebuffer terminal */
#include "cruft.h"
#include "conf.h"
#include "util.h"
#include "fb/common.h"
#include "status.h"
#include "terminal.h"
#include "ctrlseq/esc.h"
#include "ctrlseq/csi.h"
#include "ctrlseq/osc.h"
#include "ctrlseq/dcs.h"
#include "parse.h"

void sig_handler(int signo)
{
	/* global */
	extern volatile sig_atomic_t vt_active;
	extern volatile sig_atomic_t child_alive;
	extern volatile sig_atomic_t need_redraw;

	logging(DEBUG, "caught signal! no:%d\n", signo);

	if (signo == SIGCHLD) {
		child_alive = false;
		wait(NULL);
	} else if (signo == SIGTERM || signo == SIGINT) {
		/* Unblock stop/restart: exit main loop so tty_die restores KD_TEXT. */
		child_alive = false;
		vt_active = true; /* wake sigsuspend path */
	} else if (signo == SIGUSR1) { /* vt activate */
		vt_active   = true;
		need_redraw = true;
		ioctl(STDIN_FILENO, VT_RELDISP, VT_ACKACQ);
	} else if (signo == SIGUSR2) { /* vt deactivate */
		/* Only async-signal-safe work here. Canvas free + sigsuspend
		 * run in the main loop so sixel decode cannot UAF. */
		vt_active = false;
		ioctl(STDIN_FILENO, VT_RELDISP, 1);
		if (BACKGROUND_DRAW)
			need_redraw = true;
	}
}

void set_rawmode(int fd, struct termios *save_tm)
{
	struct termios tm;

	tm = *save_tm;
	tm.c_iflag     = tm.c_oflag = 0;
	tm.c_cflag    &= ~CSIZE;
	tm.c_cflag    |= CS8;
	tm.c_lflag    &= ~(ECHO | ISIG | ICANON);
	tm.c_cc[VMIN]  = 1; /* min data size (byte) */
	tm.c_cc[VTIME] = 0; /* time out */
	etcsetattr(fd, TCSAFLUSH, &tm);
}

bool tty_init(struct termios *termios_orig)
{
	struct sigaction sigact;

	memset(&sigact, 0, sizeof(struct sigaction));
	sigact.sa_handler = sig_handler;
	sigact.sa_flags   = SA_RESTART;
	esigaction(SIGCHLD, &sigact, NULL);
	esigaction(SIGTERM, &sigact, NULL);
	esigaction(SIGINT, &sigact, NULL);

	if (VT_CONTROL) {
		esigaction(SIGUSR1, &sigact, NULL);
		esigaction(SIGUSR2, &sigact, NULL);

		struct vt_mode vtm;
		vtm.mode   = VT_PROCESS;
		vtm.waitv  = 0;
		vtm.acqsig = SIGUSR1;
		vtm.relsig = SIGUSR2;
		vtm.frsig  = 0;

		if (ioctl(STDIN_FILENO, VT_SETMODE, &vtm))
			logging(WARN, "ioctl: VT_SETMODE failed (maybe here is not console)\n");

		if (FORCE_TEXT_MODE == false) {
			if (ioctl(STDIN_FILENO, KDSETMODE, KD_GRAPHICS))
				logging(WARN, "ioctl: KDSETMODE failed (maybe here is not console)\n");
		}
	}

	etcgetattr(STDIN_FILENO, termios_orig);
	set_rawmode(STDIN_FILENO, termios_orig);
	ewrite(STDIN_FILENO, "\033[?25l", 6); /* make cusor invisible */

	return true;
}

void tty_die(struct termios *termios_orig)
{
 	/* no error handling */
	struct sigaction sigact;
	struct vt_mode vtm;

	memset(&sigact, 0, sizeof(struct sigaction));
	sigact.sa_handler = SIG_DFL;
	sigaction(SIGCHLD, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGINT, &sigact, NULL);

	if (VT_CONTROL) {
		sigaction(SIGUSR1, &sigact, NULL);
		sigaction(SIGUSR2, &sigact, NULL);

		vtm.mode   = VT_AUTO;
		vtm.waitv  = 0;
		vtm.relsig = vtm.acqsig = vtm.frsig = 0;

		ioctl(STDIN_FILENO, VT_SETMODE, &vtm);

		if (FORCE_TEXT_MODE == false)
			ioctl(STDIN_FILENO, KDSETMODE, KD_TEXT);
	}

	tcsetattr(STDIN_FILENO, TCSAFLUSH, termios_orig);
	fflush(stdout);
	ewrite(STDIN_FILENO, "\033[?25h", 6); /* make cursor visible */
}

bool fork_and_exec(int *master, const char *cmd, char *const argv[], int lines, int cols)
{
	pid_t pid;
	struct winsize ws = {.ws_row = lines, .ws_col = cols,
		/* XXX: this variables are UNUSED (man tty_ioctl),
			but useful for calculating terminal cell size */
		.ws_ypixel = CELL_HEIGHT * lines, .ws_xpixel = CELL_WIDTH * cols};

	pid = eforkpty(master, NULL, NULL, &ws);
	if (pid < 0)
		return false;
	else if (pid == 0) { /* child */
		esetenv("TERM", term_name, 1);
		eexecvp(cmd, argv);
		/* never reach here */
		exit(EXIT_FAILURE);
	}
	return true;
}

int check_fds(fd_set *fds, struct timeval *tv, int input, int master)
{
	FD_ZERO(fds);
	FD_SET(input, fds);
	FD_SET(master, fds);
	tv->tv_sec  = 0;
	tv->tv_usec = SELECT_TIMEOUT;
	return eselect(master + 1, fds, NULL, NULL, tv);
}

int main(int argc, char *const argv[])
{
	extern const char *shell_cmd; /* defined in conf.h */
	const char *cmd;
	uint8_t buf[BUFSIZE];
	/* Nav prefix carried over between stdin reads: a page key can
	 * arrive split (raw VMIN=1 delivers "ESC", then "[5~") and the
	 * half must not reach the shell; the next read prepends it. */
	uint8_t navcarry[SB_NAV_HOLD_MAX];
	unsigned navcarry_n = 0;
	ssize_t size;
	fd_set fds;
	struct timeval tv;
	struct framebuffer_t fb;
	struct terminal_t term;
	/* global */
	extern volatile sig_atomic_t need_redraw;
	extern volatile sig_atomic_t child_alive;
	extern struct termios termios_orig;

	/* init */
	if (setlocale(LC_ALL, "") == NULL) /* for wcwidth() */
		logging(WARN, "setlocale falied\n");

	/* /etc/cruft.conf defaults (environment precedence); must run
	 * before the first CRUFT_* consumer below (font, status bar). */
	conf_load();

	if (!fb_init(&fb)) {
		logging(FATAL, "framebuffer initialize failed\n");
		goto fb_init_failed;
	}

	if (!glyph_mmap_init(NULL)) {
		logging(FATAL, "glyph/font initialize failed\n");
		goto glyph_init_failed;
	}

	/* Optional status bar in the top margin; no-op when the status
	 * font or config is unavailable. term_init reads statusbar_h(). */
	statusbar_init();

	if (!term_init(&term, fb.info.width, fb.info.height)) {
		logging(FATAL, "terminal initialize failed\n");
		goto term_init_failed;
	}

	if (!tty_init(&termios_orig)) {
		logging(FATAL, "tty initialize failed\n");
		goto tty_init_failed;
	}

	/* fork and exec shell */
	cmd = (argc < 2) ? shell_cmd: argv[1];
	if (!fork_and_exec(&term.fd, cmd, argv + 1, term.lines, term.cols)) {
		logging(FATAL, "forkpty failed\n");
		goto tty_init_failed;
	}
	child_alive = true;
	/* Paint erased cells immediately; don't wait for the child to speak
	 * (otherwise a post-restart FB left in KD_GRAPHICS looks blank). */
	need_redraw = true;

	/* main loop */
	while (child_alive) {
		/* VT deactivated: free transient heap, then sleep until SIGUSR1.
		 * Must not free from the signal handler (not AS-safe; races parse). */
		if (!vt_active && !BACKGROUND_DRAW) {
			sigset_t sigset;

			term_release_transient(&term);
			sigfillset(&sigset);
			/* Allow stop/restart and child exit to interrupt the wait.
			 * Blocking SIGTERM here made systemctl stop hang on inactive
			 * VTs (until SIGKILL), so font changes only hit tty1. */
			sigdelset(&sigset, SIGUSR1);
			sigdelset(&sigset, SIGTERM);
			sigdelset(&sigset, SIGINT);
			sigdelset(&sigset, SIGCHLD);
			sigsuspend(&sigset);
			continue;
		}

		if (need_redraw) {
			need_redraw = false;
			cmap_update(fb.fd, fb.cmap); /* after VT switching, need to restore cmap (in 8bpp mode) */
			redraw(&term);
			refresh(&fb, &term);
		}

		{
			int sel = check_fds(&fds, &tv, STDIN_FILENO, term.fd);

			if (sel == -1)
				continue;
			/* A carried nav prefix that never completes is a lone
			 * ESC (or dead bytes): idle stdin is the verdict.
			 * Forward it so the child loses neither the key nor
			 * its place in the stream; a genuinely split page
			 * key arrives inside the 15 ms tick. */
			if (sel == 0 && navcarry_n > 0) {
				ewrite(term.fd, navcarry, (size_t)navcarry_n);
				navcarry_n = 0;
			}
		}

		if (FD_ISSET(STDIN_FILENO, &fds)) {
			if ((size = read(STDIN_FILENO, buf, BUFSIZE)) > 0) {
				/* Nav keys (page forms, Ctrl+Home/End) at the
				 * front of the buffer steer the scrollback
				 * view and are swallowed; the rest (bare arrows
				 * among them) reaches the shell. Any foreign
				 * byte snaps the view back to the live tail.
				 * A nav prefix carried over from the previous
				 * read is prepended: the terminal may deliver
				 * one key split across reads (VMIN=1: "ESC",
				 * then "[5~"). */
				uint8_t stream[BUFSIZE + SB_NAV_HOLD_MAX];
				size_t slen = (size_t)navcarry_n + (size_t)size;
				unsigned view0 = term.sb.view;
				int eaten;
				unsigned keep = 0;

				memcpy(stream, navcarry, navcarry_n);
				memcpy(stream + navcarry_n, buf, (size_t)size);
				navcarry_n = 0;
				eaten = sb_consume_nav(&term, stream, (int)slen, &keep);
				if (slen - (size_t)eaten > (size_t)keep)
					ewrite(term.fd, stream + eaten,
					       slen - (size_t)eaten - (size_t)keep);
				if (keep > 0) {
					memcpy(navcarry, stream + (size_t)slen - keep,
					       (size_t)keep);
					navcarry_n = (unsigned)keep;
				}
				if (eaten > 0 || term.sb.view != view0)
					refresh(&fb, &term);
			}
		}
		if (FD_ISSET(term.fd, &fds)) {
			if ((size = read(term.fd, buf, BUFSIZE)) > 0) {
				if (VERBOSE)
					ewrite(STDOUT_FILENO, buf, size);
				parse(&term, buf, size);
				if (LAZY_DRAW && size == BUFSIZE)
					continue; /* maybe more data arrives soon */
				refresh(&fb, &term);
			}
		}

		/* the clock/battery tick while the pty is idle (refresh() only
		 * runs on activity); statusbar_render's once-per-second gate
		 * and content compare keep this cheap. Not while inactive:
		 * a BACKGROUND_DRAW owner has the screen. */
		if (vt_active)
			statusbar_render(&fb, &term);
	}

	/* normal exit */
	tty_die(&termios_orig);
	term_die(&term);
	statusbar_die();
	glyph_mmap_die();
	fb_die(&fb);
	return EXIT_SUCCESS;

	/* error exit */
tty_init_failed:
	term_die(&term);
term_init_failed:
	statusbar_die();
	glyph_mmap_die();
glyph_init_failed:
	fb_die(&fb);
fb_init_failed:
	return EXIT_FAILURE;
}
