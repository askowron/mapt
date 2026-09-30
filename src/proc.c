/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 APPIT Adam Skowroński
 */
#include "proc.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
	Buf line;
	int skip_lf;
} LineState;

/* Function: emit
 * Parameters:
 *   s (LineState *): Valid, writable, initialized line state whose Buf storage
 *     remains owned by the state and may be reused.
 *   cb (ProcLine): Borrowed callback function pointer; NULL disables emission.
 *   ud (void *): Borrowed opaque callback context with no imposed unit or value
 *     range; passed through unchanged.
 *   is_err (int): Dimensionless stream flag: zero denotes standard output and
 *     any nonzero value denotes standard error.
 * Return (void): No return value.  When cb is non-NULL, the current line is
 *   passed to it and its logical length is cleared.  The NUL-terminated line
 *   is borrowed and valid only for the duration of the callback.
 */
static void emit(LineState *s, ProcLine cb, void *ud, int is_err)
{
	if (!cb)
		return;
	if (!s->line.data || s->line.len == 0)
		cb(ud, "", is_err);
	else {
		s->line.data[s->line.len] = '\0';
		cb(ud, s->line.data, is_err);
	}
	s->line.len = 0;
	if (s->line.data)
		s->line.data[0] = '\0';
}

/* Function: feed
 * Parameters:
 *   s (LineState *): Valid, writable, initialized line state mutated by this
 *     function.
 *   data (const char *): Borrowed readable array of n bytes; it is not modified
 *     or retained and may be NULL only when n is zero.
 *   n (size_t): Byte count in the nonnegative size_t range.
 *   cb (ProcLine): Borrowed callback function pointer described by emit(); NULL
 *     disables callback delivery.
 *   ud (void *): Borrowed opaque callback context passed through unchanged.
 *   is_err (int): Dimensionless stream flag: zero for standard output and any
 *     nonzero value for standard error.
 * Return (void): No return value.  The bytes are split into lines; CR, LF, and
 *   CRLF delimit lines and are not included in callback strings.
 */
static void feed(LineState *s, const char *data, size_t n,
		 ProcLine cb, void *ud, int is_err)
{
	size_t i;

	for (i = 0; i < n; i++) {
		char c = data[i];

		if (c == '\r') {
			emit(s, cb, ud, is_err);
			s->skip_lf = 1;
		} else if (c == '\n') {
			if (s->skip_lf && s->line.len == 0) {
				s->skip_lf = 0;
				continue;
			}
			emit(s, cb, ud, is_err);
			s->skip_lf = 0;
		} else {
			s->skip_lf = 0;
			buf_putc(&s->line, c);
		}
	}
}

/* Function: write_all
 * Parameters:
 *   fd (int): Borrowed nonnegative Unix file descriptor already open for
 *     writing; ownership and closure remain with the caller.
 *   data (const char *): Borrowed readable array of n bytes; it is not modified
 *     or retained and may be NULL only when n is zero.
 *   n (size_t): Byte count in the nonnegative size_t range.
 * Return (void): No return value.  Attempts to write all bytes, retries after
 *   signal interruption, and stops at other write errors.
 */
static void write_all(int fd, const char *data, size_t n)
{
	while (n) {
		ssize_t k = write(fd, data, n);

		if (k < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		data += k;
		n -= (size_t)k;
	}
}

/* Function: proc_run
 * Parameters:
 *   argv (char *const *): Borrowed NULL-terminated vector of string pointers;
 *     argv and argv[0] must be non-NULL, and every used entry must address a
 *     NUL-terminated string.  The caller retains ownership, and the vector and
 *     strings remain valid and unmodified until this call returns.  argv[0]
 *     is searched on PATH when it contains no slash.
 *   stdin_data (const char *): Borrowed NUL-terminated byte string whose
 *     strlen() gives the input length in bytes, or NULL to connect the child
 *     to /dev/null; it is not modified or retained.
 *   on_line (ProcLine): Borrowed callback function pointer, or NULL.  Each
 *     line passed to it is a borrowed NUL-terminated string valid only for
 *     that callback invocation, including for an empty line; is_err is zero
 *     for standard output and any nonzero value for standard error.
 *   on_tick (ProcTick): Borrowed callback function pointer, or NULL, invoked
 *     synchronously while the child runs; any nonzero return requests
 *     cancellation with SIGTERM.
 *   ud (void *): Borrowed, caller-owned opaque context with no imposed unit or
 *     value range; passed unchanged to callbacks and otherwise subject to the
 *     caller's lifetime and ownership rules.
 * Return (int): The child's exit code in the range 0 through 255 when it exits
 *   normally, 128 plus its terminating signal number when signaled, or -1 for
 *   an invalid vector, setup/fork failure, or wait failure.  A child-side
 *   execvp() failure is reported as exit status 127.
 */
int proc_run(char *const argv[], const char *stdin_data,
	     ProcLine on_line, ProcTick on_tick, void *ud)
{
	int op[2] = { -1, -1 }, ep[2] = { -1, -1 }, ip[2] = { -1, -1 };
	pid_t pid;
	int out_open = 1, err_open = 1;
	int cancelled = 0, kills = 0;
	LineState ls[2];
	char rbuf[8192];
	int status;

	if (!argv || !argv[0])
		return -1;
	if (pipe(op) < 0)
		return -1;
	if (pipe(ep) < 0) {
		close(op[0]);
		close(op[1]);
		return -1;
	}
	if (stdin_data && pipe(ip) < 0) {
		close(op[0]);
		close(op[1]);
		close(ep[0]);
		close(ep[1]);
		return -1;
	}

	pid = fork();
	if (pid < 0) {
		close(op[0]);
		close(op[1]);
		close(ep[0]);
		close(ep[1]);
		if (ip[0] >= 0) {
			close(ip[0]);
			close(ip[1]);
		}
		return -1;
	}

	if (pid == 0) {
		int devnull;

		/* Everything we parse (apt-cache policy, dpkg-query) uses
		 * fixed English field names, so force the C locale and make
		 * package operations non interactive. */
		setenv("LC_ALL", "C", 1);
		setenv("LANG", "C", 1);
		setenv("LANGUAGE", "", 1);
		setenv("DEBIAN_FRONTEND", "noninteractive", 1);
		setenv("APT_LISTCHANGES_FRONTEND", "none", 1);
		setenv("NEEDRESTART_MODE", "a", 1);

		if (dup2(op[1], STDOUT_FILENO) < 0 ||
		    dup2(ep[1], STDERR_FILENO) < 0)
			_exit(127);
		if (stdin_data) {
			if (dup2(ip[0], STDIN_FILENO) < 0)
				_exit(127);
		} else {
			devnull = open("/dev/null", O_RDONLY);
			if (devnull >= 0) {
				dup2(devnull, STDIN_FILENO);
				close(devnull);
			}
		}
		close(op[0]);
		close(op[1]);
		close(ep[0]);
		close(ep[1]);
		if (ip[0] >= 0)
			close(ip[0]);
		if (ip[1] >= 0)
			close(ip[1]);
		execvp(argv[0], argv);
		dprintf(STDERR_FILENO, "mapt: %s: %s\n", argv[0],
			strerror(errno));
		_exit(127);
	}

	close(op[1]);
	close(ep[1]);
	if (stdin_data) {
		close(ip[0]);
		write_all(ip[1], stdin_data, strlen(stdin_data));
		close(ip[1]);
	} else if (ip[0] >= 0) {
		close(ip[0]);
		close(ip[1]);
	}

	buf_init(&ls[0].line);
	buf_init(&ls[1].line);
	ls[0].skip_lf = ls[1].skip_lf = 0;

	while (out_open || err_open) {
		struct pollfd pf[2];
		int nfds = 0, oi = -1, ei = -1, r;

		if (out_open) {
			oi = nfds;
			pf[nfds].fd = op[0];
			pf[nfds].events = POLLIN | POLLHUP;
			nfds++;
		}
		if (err_open) {
			ei = nfds;
			pf[nfds].fd = ep[0];
			pf[nfds].events = POLLIN | POLLHUP;
			nfds++;
		}

		r = poll(pf, (nfds_t)nfds, on_tick ? 150 : -1);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0) {
			if (on_tick && on_tick(ud)) {
				if (!cancelled) {
					kill(pid, SIGTERM);
					cancelled = 1;
				}
			} else if (cancelled && ++kills > 20) {
				kill(pid, SIGKILL);
				kills = 0;
			}
			continue;
		}

		if (oi >= 0 && (pf[oi].revents & (POLLIN | POLLHUP | POLLERR))) {
			ssize_t k = read(op[0], rbuf, sizeof(rbuf));

			if (k > 0)
				feed(&ls[0], rbuf, (size_t)k, on_line, ud, 0);
			else if (k == 0 ||
				 (k < 0 && errno != EINTR && errno != EAGAIN)) {
				out_open = 0;
				close(op[0]);
			}
		}
		if (ei >= 0 && (pf[ei].revents & (POLLIN | POLLHUP | POLLERR))) {
			ssize_t k = read(ep[0], rbuf, sizeof(rbuf));

			if (k > 0)
				feed(&ls[1], rbuf, (size_t)k, on_line, ud, 1);
			else if (k == 0 ||
				 (k < 0 && errno != EINTR && errno != EAGAIN)) {
				err_open = 0;
				close(ep[0]);
			}
		}

		/* Let the UI react to keys even while the child is
		 * producing a steady stream of output. */
		if (on_tick && on_tick(ud)) {
			if (!cancelled) {
				kill(pid, SIGTERM);
				cancelled = 1;
			}
		} else if (cancelled && ++kills > 40) {
			kill(pid, SIGKILL);
			kills = 0;
		}
	}

	if (ls[0].line.len)
		emit(&ls[0], on_line, ud, 0);
	if (ls[1].line.len)
		emit(&ls[1], on_line, ud, 1);
	buf_free(&ls[0].line);
	buf_free(&ls[1].line);

	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}

	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return -1;
}
