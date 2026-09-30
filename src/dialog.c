/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 APPIT Adam Skowroński
 */
#include "dialog.h"
#include "proc.h"
#include "ui.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ *
 * Helpers                                                             *
 * ------------------------------------------------------------------ */

/*
 * Function: Close a popup window and put the background it covered back.
 * Parameters:
 *   win (WINDOW *): Window to destroy; NULL is ignored.
 * Return (void): No return value.
 */
static void close_win(WINDOW *win)
{
	if (!win)
		return;
	ui_unshadow(win);
	delwin(win);
	touchwin(stdscr);
}

static WINDOW *mkwin_at(int h, int w, int y, int x)
{
	WINDOW *win;

	if (h > LINES)
		h = LINES;
	if (w > COLS)
		w = COLS;
	if (h < 3)
		h = LINES < 3 ? LINES : 3;
	if (w < 8)
		w = COLS < 8 ? COLS : 8;
	if (y < 0)
		y = 0;
	if (x < 0)
		x = 0;
	if (y + h > LINES)
		y = LINES - h;
	if (x + w > COLS)
		x = COLS - w;
	if (y < 0)
		y = 0;
	if (x < 0)
		x = 0;

	win = newwin(h, w, y, x);
	if (!win)
		return NULL;
	wbkgd(win, CP(CP_DLG));
	keypad(win, TRUE);
	ui_shadow(win);
	return win;
}

static WINDOW *mkwin_centered(int h, int w, int *py, int *px)
{
	int y = (LINES - h) / 2;
	int x = (COLS - w) / 2;

	if (py)
		*py = y;
	if (px)
		*px = x;
	return mkwin_at(h, w, y, x);
}

/*
 * Function: Find the longest wrapped line width in a text block.
 * Parameters:
 *   text (const char *): Text possibly containing newlines.
 * Return (int): Maximum line width in display cells.
 */
static int text_width(const char *text)
{
	const char *p = text;
	int max = 0;

	while (*p) {
		const char *nl = strchr(p, '\n');
		int len = nl ? (int)(nl - p) : (int)strlen(p);

		if (len > max)
			max = len;
		p = nl ? nl + 1 : p + len;
	}
	return max;
}

/*
 * Function: Draw the footer of a dialog: a bright horizontal rule that
 *           separates the body from the key hint below it.
 * Parameters:
 *   win (WINDOW *): Target window; the rule goes one row above the hint.
 *   hint (const char *): Key hint text, or NULL for the rule alone.
 * Return (void): No return value.
 */
static void draw_footer(WINDOW *win, const char *hint)
{
	int attr = CP(CP_SEL) | A_BOLD;
	int h, w;

	getmaxyx(win, h, w);
	if (h < 5 || w < 6)
		return;
	/* Cyan reads as a bright rule on the white body of a dialog and on
	 * the red warning background of the input dialog alike. */
	wattron(win, attr);
	mvwhline(win, h - 4, 1, ACS_HLINE, w - 2);
	wattroff(win, attr);
	if (hint)
		mvwaddnstr(win, h - 3, 2, hint, (size_t)(w - 4));
}

/*
 * Function: Draw a bounded text line into a window.
 * Parameters:
 *   win (WINDOW *): Target window.
 *   y (int): Target row.
 *   x (int): Starting column.
 *   maxw (int): Maximum number of cells to draw.
 *   s (const char *): Text to draw.
 * Return (void): No return value.
 */
static void put_line(WINDOW *win, int y, int x, int maxw, const char *s)
{
	char buf[1024];

	if (maxw < 1)
		return;
	fit_into(buf, sizeof(buf) < (size_t)maxw + 1 ?
			 sizeof(buf) : (size_t)maxw + 1, s);
	mvwaddnstr(win, y, x, buf, maxw);
}

/* Like put_line but for a span that is not NUL terminated (it may be
 * followed by more data in the same buffer). */
/*
 * Function: Draw a bounded byte span as a temporary NUL-terminated line.
 * Parameters:
 *   win (WINDOW *): Target window.
 *   y (int): Target row.
 *   x (int): Starting column.
 *   maxw (int): Maximum number of cells to draw.
 *   s (const char *): Beginning of the source span.
 *   span (size_t): Number of source bytes available at s.
 * Return (void): No return value.
 */
static void put_span(WINDOW *win, int y, int x, int maxw, const char *s,
		     size_t span)
{
	char tmp[2048];
	size_t n = span;

	if (maxw < 1)
		return;
	if (n >= sizeof(tmp))
		n = sizeof(tmp) - 1;
	memcpy(tmp, s, n);
	tmp[n] = '\0';
	put_line(win, y, x, maxw, tmp);
}

/*
 * Function: Translate a wheel event into a signed vertical direction.
 * Parameters:
 *   ev (MEVENT *): Mouse event whose bstate may contain a wheel button.
 * Return (int): -1 for up, 1 for down, or 0 for a non-wheel event.
 */
static int mouse_wheel_dir(const MEVENT *ev)
{
	if (ev->bstate & BUTTON4_PRESSED)
		return -1;
	if (ev->bstate & BUTTON5_PRESSED)
		return 1;
	return 0;
}

/*
 * Function: Convert a screen-relative event to window coordinates and bounds-check it.
 * Parameters:
 *   win (WINDOW *): Popup window receiving the event.
 *   ev (MEVENT *): Screen-relative mouse event to convert.
 *   py (int *): Receives the zero-based window row.
 *   px (int *): Receives the zero-based window column.
 * Return (int): Non-zero when the event lies inside win.
 */
static int mouse_in_window(WINDOW *win, const MEVENT *ev, int *py, int *px)
{
	int wy, wx, h, w;

	getbegyx(win, wy, wx);
	getmaxyx(win, h, w);
	*py = ev->y - wy;
	*px = ev->x - wx;
	return *py >= 0 && *py < h && *px >= 0 && *px < w;
}

static void draw_button(WINDOW *win, int y, int x, const char *label,
			int selected)
{
	int a = selected ? CP(CP_SEL) : (CP(CP_DLG) | A_BOLD);

	wattron(win, a);
	mvwprintw(win, y, x, "[ %s ]", label);
	wattroff(win, a);
}

/* Wrap text into an array of heap allocated lines of at most width
 * characters.  Returns a NULL terminated array; caller frees it. */
static char **wrap_lines(const char *text, int width, int *count)
{
	char **out = NULL;
	size_t n = 0, cap = 0;
	const char *s = text ? text : "";

	if (width < 8)
		width = 8;

	while (*s) {
		const char *e = strchr(s, '\n');
		size_t rawlen = e ? (size_t)(e - s) : strlen(s);
		size_t off = 0;

		while (rawlen && (s[rawlen - 1] == '\r' || s[rawlen - 1] == ' '))
			rawlen--;

		if (rawlen == 0) {
			if (n + 1 > cap) {
				cap = cap ? cap * 2 : 32;
				out = xrealloc(out, cap * sizeof(*out));
			}
			out[n++] = xstrdup("");
		} else {
			while (off < rawlen) {
				size_t rest = rawlen - off;
				size_t brk;

				if (n + 1 > cap) {
					cap = cap ? cap * 2 : 32;
					out = xrealloc(out, cap * sizeof(*out));
				}
				if (rest <= (size_t)width) {
					out[n++] = xstrndup(s + off, rest);
					break;
				}
				brk = (size_t)width;
				while (brk > 0 && s[off + brk] != ' ')
					brk--;
				if (brk == 0)
					brk = (size_t)width;
				out[n++] = xstrndup(s + off, brk);
				off += brk;
				while (off < rawlen && s[off] == ' ')
					off++;
			}
		}
		if (!e)
			break;
		s = e + 1;
	}
	if (n + 1 > cap) {
		cap += 1;
		out = xrealloc(out, cap * sizeof(*out));
	}
	out[n] = NULL;
	*count = (int)n;
	return out;
}

/*
 * Function: Free a NULL-terminated array of wrapped text lines.
 * Parameters:
 *   lines (char **): Array returned by wrap_lines(), or NULL.
 * Return (void): No return value.
 */
static void free_lines(char **lines)
{
	int i;

	if (!lines)
		return;
	for (i = 0; lines[i]; i++)
		free(lines[i]);
	free(lines);
}

/* Modal loops read from stdscr: ncurses places mouse reports there even
 * when the visible input is a popup window. */

/* ------------------------------------------------------------------ *
 * Alert / confirm                                                      *
 * ------------------------------------------------------------------ */

/*
 * Function: Display a modal alert with one acknowledgement button.
 * Parameters:
 *   title (const char *): Popup title.
 *   body (const char *): Message text to wrap and display.
 *   is_error (int): Non-zero selects the red error title style.
 * Return (void): No return value.
 */
void dlg_alert(const char *title, const char *body, int is_error)
{
	int bw = text_width(body);
	int tw = title ? (int)strlen(title) : 0;
	int w = bw + 4;
	int h, i, n = 0;
	char **lines = NULL;
	WINDOW *win;
	int bx;

	if (w < tw + 6)
		w = tw + 6;
	if (w < 26)
		w = 26;
	if (w > COLS - 2)
		w = COLS - 2;

	lines = wrap_lines(body, w - 4, &n);
	h = n + 6; /* border, pad, lines, gap, button, pad */
	if (h > LINES - 2)
		h = LINES < 3 ? LINES : LINES - 2;
	if (h < 7)
		h = 7;

	win = mkwin_centered(h, w, NULL, NULL);
	if (!win) {
		free_lines(lines);
		return;
	}
	box(win, 0, 0);
	wattron(win, is_error ? CP(CP_ERR) | A_BOLD : CP(CP_SEL) | A_BOLD);
	mvwprintw(win, 0, 2, " %s ", title ? title : "Message");
	wattroff(win, is_error ? CP(CP_ERR) | A_BOLD : CP(CP_SEL) | A_BOLD);

	for (i = 0; i < n && i < h - 5; i++)
		put_line(win, 2 + i, 2, w - 4, lines[i]);

	bx = (w - 8) / 2;
	draw_button(win, h - 3, bx, "OK", 1);
	wrefresh(win);
	free_lines(lines);

	for (;;) {
		int c = wgetch(stdscr);

		if (c == KEY_MOUSE) {
			MEVENT ev;
			int my, mx;

			if (getmouse(&ev) == OK) {
				int left = ui_mouse_clicked(&ev, 1);

				if (left && mouse_in_window(win, &ev, &my, &mx) &&
				    my == h - 3 && mx >= bx && mx < bx + 6)
					break;
			}
			continue;
		}
		if (c != ERR && c != KEY_RESIZE)
			break;
	}
	close_win(win);
}

/*
 * Function: Display a modal Yes/No confirmation dialog.
 * Parameters:
 *   title (const char *): Popup title.
 *   body (const char *): Confirmation message to wrap and display.
 *   default_yes (int): Non-zero initially highlights Yes.
 * Return (int): 1 for Yes, 0 for No or cancellation.
 */
int dlg_confirm(const char *title, const char *body, int default_yes)
{
	int bw = text_width(body);
	int tw = title ? (int)strlen(title) : 0;
	int w = bw + 4;
	int h, i, n = 0, sel = default_yes ? 0 : 1;
	char **lines;
	WINDOW *win;
	int total = (int)strlen("[ Yes ]") + (int)strlen("[ No ]") + 1;
	int bx;

	if (w < tw + 6)
		w = tw + 6;
	if (w < 30)
		w = 30;
	if (w > COLS - 2)
		w = COLS - 2;

	lines = wrap_lines(body, w - 4, &n);
	h = n + 6; /* border, pad, lines, gap, buttons, pad */
	if (h < 7)
		h = 7;
	if (h > LINES - 2)
		h = LINES < 3 ? LINES : LINES - 2;

	win = mkwin_centered(h, w, NULL, NULL);
	if (!win) {
		free_lines(lines);
		return 0;
	}
	box(win, 0, 0);
	wattron(win, CP(CP_SEL) | A_BOLD);
	mvwprintw(win, 0, 2, " %s ", title ? title : "Confirm");
	wattroff(win, CP(CP_SEL) | A_BOLD);

	for (i = 0; i < n && i < h - 5; i++)
		put_line(win, 2 + i, 2, w - 4, lines[i]);
	free_lines(lines);

	bx = (w - total) / 2;
	if (bx < 2)
		bx = 2;

	for (;;) {
		draw_button(win, h - 3, bx, "Yes", sel == 0);
		draw_button(win, h - 3, bx + (int)strlen("[ Yes ]") + 1,
			    "No", sel == 1);
		wrefresh(win);

		{
			int c = wgetch(stdscr);

			switch (c) {
			case KEY_MOUSE: {
				MEVENT ev;
				int my, mx;

				if (getmouse(&ev) == OK) {
					int left = ui_mouse_clicked(&ev, 1);

					if (left &&
					    mouse_in_window(win, &ev, &my, &mx) &&
					    my == h - 3) {
						int nox = bx +
							  (int)strlen("[ Yes ]") + 1;

						if (mx >= bx &&
						    mx < bx +
								(int)strlen("[ Yes ]")) {
							close_win(win);
							return 1;
						}
						if (mx >= nox &&
						    mx < nox +
								(int)strlen("[ No ]")) {
							close_win(win);
							return 0;
						}
					}
				}
				break;
			}
			case KEY_LEFT:
			case KEY_BTAB:
				sel = 0;
				break;
			case KEY_RIGHT:
			case '\t':
				sel = 1;
				break;
			case 'y':
			case 'Y':
				close_win(win);
				return 1;
			case 'n':
			case 'N':
				close_win(win);
				return 0;
			case '\n':
			case KEY_ENTER:
				close_win(win);
				return sel == 0;
			case 27:
			case KEY_F(10):
				close_win(win);
				return 0;
			default:
				break;
			}
		}
	}
}

/* ------------------------------------------------------------------ *
 * Single line input                                                    *
 * ------------------------------------------------------------------ */

/*
 * Function: Edit one line in a popup field.
 * Parameters:
 *   title (char *): Popup title.
 *   prompt (char *): Optional label displayed above the field.
 *   buf (char *): Buffer receiving the edited string.
 *   cap (size_t): Capacity of buf in bytes, including the terminating NUL.
 *   hidden (int): Non-zero masks entered characters with asterisks.
 *   danger (int): Non-zero selects the red authentication style.
 * Return (int): 0 when accepted, or -1 when cancelled/invalid.
 */
static int input_dialog(const char *title, const char *prompt, char *buf,
			size_t cap, int hidden, int danger)
{
	int w, h, field_row, hint_row, pos;
	int field_x = 3;
	int field_w, off;
	WINDOW *win;
	size_t len;

	if (!buf || !cap)
		return -1;
	len = strlen(buf);
	pos = (int)len;

	w = 40;
	if (prompt) {
		int pl = (int)strlen(prompt);

		if (pl + 6 > w)
			w = pl + 6;
	}
	if (w > COLS - 2)
		w = COLS - 2;
	if (w < 20)
		w = COLS;

	/* Leave one blank cell on every side of the input field. */
	field_row = prompt ? 4 : 3;
	hint_row = field_row + 2;
	h = hint_row + 3;
	while (h > LINES && field_row > 2) {
		field_row--;
		hint_row--;
		h--;
	}
	if (w < field_x + 5)
		field_x = 2;

	win = mkwin_centered(h, w, NULL, NULL);
	if (!win)
		return -1;

	if (danger)
		wbkgd(win, CP(CP_ERR));
	box(win, 0, 0);
	wattron(win, (danger ? CP(CP_ERR) : CP(CP_SEL)) | A_BOLD);
	mvwprintw(win, 0, 2, " %s ", title ? title : "Input");
	wattroff(win, (danger ? CP(CP_ERR) : CP(CP_SEL)) | A_BOLD);
	if (prompt)
		mvwaddnstr(win, field_row > 2 ? field_row - 2 : 1, 2,
			   prompt, (size_t)(w - 4));
	draw_footer(win, "[Enter] OK   [Esc] Cancel");

	field_w = w - field_x - 3;
	if (field_w < 4)
		field_w = 4;
	curs_set(1);

	for (;;) {
		int i;

		/* field background */
		wattron(win, CP(CP_SEL));
		for (i = 0; i < field_w; i++)
			mvwaddch(win, field_row, field_x + i, ' ');
		wattroff(win, CP(CP_SEL));

		off = pos >= field_w ? pos - field_w + 1 : 0;
		for (i = 0; i < field_w; i++) {
			int idx = off + i;

			if (idx >= (int)len)
				break;
			mvwaddch(win, field_row, field_x + i,
				 hidden ? '*' : (chtype)(unsigned char)buf[idx]);
		}
		wmove(win, field_row, field_x + (pos - off));
		wrefresh(win);

		{
			int c = wgetch(stdscr);

			switch (c) {
			case KEY_MOUSE: {
				MEVENT ev;
				int my, mx;

				if (getmouse(&ev) == OK) {
					int left = ui_mouse_clicked(&ev, 1);

					if (left &&
					    mouse_in_window(win, &ev, &my, &mx) &&
					    my == field_row && mx >= field_x &&
					    mx < field_x + field_w) {
						pos = off + mx - field_x;
						if (pos > (int)len)
							pos = (int)len;
					}
				}
				break;
			}
			case '\n':
			case KEY_ENTER:
				curs_set(0);
				close_win(win);
				return 0;
			case 27:
			case KEY_F(10):
				curs_set(0);
				close_win(win);
				return -1;
			case KEY_BACKSPACE:
			case 127:
			case 8:
				if (pos > 0) {
					memmove(buf + pos - 1, buf + pos,
						len - (size_t)pos + 1);
					pos--;
					len--;
				}
				break;
			case KEY_DC:
				if ((size_t)pos < len) {
					memmove(buf + pos, buf + pos + 1,
						len - (size_t)pos);
					len--;
				}
				break;
			case KEY_LEFT:
				if (pos > 0)
					pos--;
				break;
			case KEY_RIGHT:
				if (pos < (int)len)
					pos++;
				break;
			case KEY_HOME:
				pos = 0;
				break;
			case KEY_END:
				pos = (int)len;
				break;
			default:
				if (c >= 32 && c < 256 &&
				    len + 1 < cap) {
					memmove(buf + pos + 1, buf + pos,
						len - (size_t)pos + 1);
					buf[pos++] = (char)c;
					len++;
				}
				break;
			}
		}
	}
}

/*
 * Function: Open a normal single-line input field.
 * Parameters:
 *   title (char *): Popup title.
 *   prompt (char *): Optional label displayed above the field.
 *   buf (char *): Buffer receiving the edited string.
 *   cap (size_t): Capacity of buf in bytes, including the terminating NUL.
 *   hidden (int): Non-zero masks entered characters with asterisks.
 * Return (int): 0 when accepted, or -1 when cancelled/invalid.
 */
int dlg_input(const char *title, const char *prompt, char *buf, size_t cap,
	      int hidden)
{
	return input_dialog(title, prompt, buf, cap, hidden, 0);
}

/*
 * Function: Open a hidden field with the red authentication styling.
 * Parameters:
 *   title (char *): Popup title.
 *   prompt (char *): Optional label displayed above the field.
 *   buf (char *): Buffer receiving the password.
 *   cap (size_t): Capacity of buf in bytes, including the terminating NUL.
 * Return (int): 0 when accepted, or -1 when cancelled/invalid.
 */
int dlg_password(const char *title, const char *prompt, char *buf, size_t cap)
{
	return input_dialog(title, prompt, buf, cap, 1, 1);
}

/* ------------------------------------------------------------------ *
 * Scrollable text viewer                                               *
 * ------------------------------------------------------------------ */

/*
 * Function: Draw the visible portion of a wrapped text viewer.
 * Parameters:
 *   win (WINDOW *): Target viewer window.
 *   lines (char **): Array of n wrapped text lines.
 *   n (int): Number of wrapped lines.
 *   top (int): Index of the first visible line.
 *   vis (int): Number of visible text rows.
 * Return (void): No return value.
 */
static void text_draw(WINDOW *win, char **lines, int n, int top, int vis)
{
	int i, w, h;

	getmaxyx(win, h, w);
	(void)h;
	for (i = 0; i < vis; i++) {
		int idx = top + i;

		wmove(win, 2 + i, 1);
		wclrtoeol(win);
		mvwaddch(win, 2 + i, w - 1, ACS_VLINE);
		if (idx < n)
			put_line(win, 2 + i, 2, w - 4, lines[idx]);
	}
	/* The right border is a one-cell scrollbar; keep its thumb in
	 * proportion to the current top row. */
	if (n > vis) {
		int max_top = n - vis;
		int by = 2 + (vis > 1 ?
				      (int)((long)top * (vis - 1) / max_top) :
				      0);

		wattron(win, A_REVERSE);
		mvwaddch(win, by, w - 1, ACS_CKBOARD);
		wattroff(win, A_REVERSE);
	}
	{
		char right[48];
		int last = top + vis;

		if (last > n)
			last = n;
		snprintf(right, sizeof(right), " %d-%d/%d ", top + 1, last, n);
		wmove(win, 0, w - 2 - (int)strlen(right));
		wclrtoeol(win);
		mvwaddch(win, 0, w - 1, ACS_URCORNER);
		wattron(win, CP(CP_SEL));
		mvwaddstr(win, 0, w - 2 - (int)strlen(right), right);
		wattroff(win, CP(CP_SEL));
	}
}

/*
 * Function: Display text in a scrollable modal viewer.
 * Parameters:
 *   title (const char *): Popup title.
 *   text (const char *): Text to wrap and display.
 * Return (void): No return value.
 */
void dlg_text(const char *title, const char *text)
{
	int w = COLS - 6;
	int h = LINES - 4;
	int n = 0, top = 0, vis;
	char **lines;
	WINDOW *win;

	if (w > 96)
		w = 96;
	if (w < 24)
		w = COLS;
	if (h > 32)
		h = 32;
	if (h < 6)
		h = LINES;

	lines = wrap_lines(text, w - 4, &n);
	win = mkwin_centered(h, w, NULL, NULL);
	if (!win) {
		free_lines(lines);
		return;
	}
	box(win, 0, 0);
	wattron(win, CP(CP_SEL) | A_BOLD);
	mvwprintw(win, 0, 2, " %s ", title ? title : "Text");
	wattroff(win, CP(CP_SEL) | A_BOLD);
	draw_footer(win,
		    "arrows/wheel/PgUp/PgDn scroll   Enter/Esc/q close");

	/* One row shorter than the body: the last one belongs to the
	 * separator rule, which the hint line used to overlap. */
	vis = h - 6;
	if (vis < 1)
		vis = 1;
	if (top > n - vis)
		top = n - vis;
	if (top < 0)
		top = 0;

	text_draw(win, lines, n, top, vis);
	wrefresh(win);

	for (;;) {
		int c = wgetch(stdscr);
		int moved = 0;

		switch (c) {
		case KEY_MOUSE: {
			MEVENT ev;
			int my, mx, wheel, left;

			if (getmouse(&ev) != OK)
				break;
			left = ui_mouse_clicked(&ev, 1);
			if (!mouse_in_window(win, &ev, &my, &mx))
				break;
			wheel = mouse_wheel_dir(&ev);
			if (wheel && my >= 2 && my < 2 + vis) {
				int step = vis < 3 ? vis : 3;

				top += wheel * step;
				moved = 1;
			} else if (left && mx == w - 1 && my >= 2 &&
				   my < 2 + vis && n > vis) {
				if (vis > 1)
					top = (int)((long)(n - vis) *
						    (my - 2) / (vis - 1));
				else
					top = 0;
				moved = 1;
			}
			break;
		}
		case KEY_DOWN:
			if (top < n - vis) {
				top++;
				moved = 1;
			}
			break;
		case KEY_UP:
			if (top > 0) {
				top--;
				moved = 1;
			}
			break;
		case KEY_NPAGE:
		case ' ':
			if (top + vis < n) {
				top += vis;
				if (top > n - vis)
					top = n - vis;
				moved = 1;
			}
			break;
		case KEY_PPAGE:
			top -= vis;
			if (top < 0)
				top = 0;
			moved = 1;
			break;
		case KEY_HOME:
			top = 0;
			moved = 1;
			break;
		case KEY_END:
			top = n - vis;
			if (top < 0)
				top = 0;
			moved = 1;
			break;
		case 'j':
			if (top < n - vis) {
				top++;
				moved = 1;
			}
			break;
		case 'k':
			if (top > 0) {
				top--;
				moved = 1;
			}
			break;
		case 27:
		case '\n':
		case KEY_ENTER:
		case KEY_F(10):
		case 'q':
			close_win(win);
			free_lines(lines);
			return;
		default:
			if (c == KEY_RESIZE)
				moved = 1;
			break;
		}
		if (moved) {
			werase(win);
			box(win, 0, 0);
			wattron(win, CP(CP_SEL) | A_BOLD);
			mvwprintw(win, 0, 2, " %s ",
				  title ? title : "Text");
			wattroff(win, CP(CP_SEL) | A_BOLD);
			draw_footer(win,
				    "arrows/wheel/PgUp/PgDn scroll   Enter/Esc/q close");
			if (top > n - vis)
				top = n - vis;
			if (top < 0)
				top = 0;
			text_draw(win, lines, n, top, vis);
		}
		wrefresh(win);
	}
}

/* ------------------------------------------------------------------ *
 * Dropdown menu                                                        *
 * ------------------------------------------------------------------ */

/*
 * Function: Test whether a menu label represents a separator.
 * Parameters:
 *   s (const char *): Menu label to inspect.
 * Return (int): Non-zero for the separator label.
 */
static int is_sep(const char *s)
{
	return s && strcmp(s, "---") == 0;
}

/*
 * Function: Display an interactive dropdown menu.
 * Parameters:
 *   items (const char *const *): NULL-terminated array of n item labels.
 *   n (int): Number of item labels in items.
 *   initial (int): Initially highlighted item index.
 *   y (int): Requested screen row for the dropdown.
 *   x (int): Requested screen column for the dropdown.
 *   top_x (int *): Receives a top-bar column when that bar is clicked.
 * Return (int): Item index, navigation code, or DLG_MENU_CANCEL.
 */
int dlg_menu(const char *const *items, int n, int initial, int y, int x,
	     int *top_x)
{
	int w = 12, i, cur = initial, h;
	WINDOW *win;

	if (top_x)
		*top_x = -1;
	if (n <= 0)
		return DLG_MENU_CANCEL;
	for (i = 0; i < n; i++) {
		int l = (int)strlen(items[i]);

		if (l > w)
			w = l;
	}
	w += 4;
	h = n + 4; /* border, pad, items, pad */

	if (y + h > LINES)
		y = LINES - h;
	if (x + w > COLS)
		x = COLS - w;

	win = mkwin_at(h, w, y, x);
	if (!win)
		return DLG_MENU_CANCEL;
	box(win, 0, 0);

	if (cur < 0 || cur >= n || is_sep(items[cur])) {
		cur = 0;
		while (cur < n && is_sep(items[cur]))
			cur++;
		if (cur >= n)
			cur = 0;
	}

	for (;;) {
		for (i = 0; i < n; i++) {
			int r = i + 2; /* first row after the pad */

			if (is_sep(items[i])) {
				mvwhline(win, r, 1, ACS_HLINE, w - 2);
				continue;
			}
			if (i == cur) {
				int a = CP(CP_SEL);

				wattron(win, a);
				{
					int k;

					for (k = 1; k < w - 1; k++)
						mvwaddch(win, r, k, ' ');
				}
				mvwaddnstr(win, r, 2, items[i],
					   (size_t)(w - 4));
				wattroff(win, a);
			} else {
				wmove(win, r, 1);
				wclrtoeol(win);
				mvwaddch(win, r, w - 1, ACS_VLINE);
				mvwaddnstr(win, r, 2, items[i],
					   (size_t)(w - 4));
			}
		}
		wrefresh(win);

		{
			int c = wgetch(stdscr);
			int prev = cur;

			switch (c) {
			case KEY_MOUSE: {
				MEVENT ev;
				int my, mx, wheel, mh, mw, left;

				if (getmouse(&ev) != OK)
					break;
				left = ui_mouse_clicked(&ev, 1);
				/* A click on row zero belongs to the top menu bar,
				 * not to the dropdown window. */
				if (ev.y == 0) {
					if (left) {
						if (top_x)
							*top_x = ev.x;
						close_win(win);
						return DLG_MENU_TOP;
					}
					break;
				}
				if (!mouse_in_window(win, &ev, &my, &mx)) {
					if (left) {
						close_win(win);
						return DLG_MENU_CANCEL;
					}
					break;
				}
				getmaxyx(win, mh, mw);
				wheel = mouse_wheel_dir(&ev);
				if (wheel) {
					do {
						cur = wheel < 0 ?
							      (cur > 0 ? cur - 1 :
								       n - 1) :
							      (cur + 1) % n;
					} while (is_sep(items[cur]));
				} else if (left && mx >= 1 && mx < mw - 1 &&
					   my >= 2 && my < mh - 1) {
					int idx = my - 2;

					if (idx >= 0 && idx < n &&
					    !is_sep(items[idx])) {
						close_win(win);
						return idx;
					}
				}
				break;
			}
			case KEY_UP:
				do {
					cur = cur > 0 ? cur - 1 : n - 1;
				} while (is_sep(items[cur]));
				break;
			case KEY_DOWN:
				do {
					cur = (cur + 1) % n;
				} while (is_sep(items[cur]));
				break;
			case KEY_PPAGE:
				cur = 0;
				while (cur < n && is_sep(items[cur]))
					cur++;
				break;
			case KEY_NPAGE:
				cur = n - 1;
				while (cur > 0 && is_sep(items[cur]))
					cur--;
				break;
			case KEY_HOME:
				cur = 0;
				while (cur < n && is_sep(items[cur]))
					cur++;
				break;
			case KEY_END:
				cur = n - 1;
				while (cur > 0 && is_sep(items[cur]))
					cur--;
				break;
			case KEY_LEFT:
				close_win(win);
				return DLG_MENU_LEFT;
			case KEY_RIGHT:
				close_win(win);
				return DLG_MENU_RIGHT;
			case '\n':
			case KEY_ENTER:
				close_win(win);
				return is_sep(items[cur]) ? DLG_MENU_CANCEL
							  : cur;
			case 27:
			case KEY_F(9):
			case KEY_F(10):
				close_win(win);
				return DLG_MENU_CANCEL;
			default:
				if (c == KEY_RESIZE)
					prev = -1;
				break;
			}
			if (prev != cur)
				continue;
		}
	}
}

/* ------------------------------------------------------------------ *
 * Command output window                                                *
 * ------------------------------------------------------------------ */

typedef struct {
	WINDOW *win;
	Buf out;
	size_t *off;
	size_t n_off, cap_off;
	size_t top;
	int at_tail;
	int oh; /* output rows */
	int status;
	int pct; /* progress percentage, -1 when unknown */
	long t0; /* mono_ms() at start, drives the spinner */
	time_t start;
	time_t last_sec;
} CmdCtx;

/*
 * Function: Compute the first output row at the live tail.
 * Parameters:
 *   c (CmdCtx *): Command-window state.
 * Return (size_t): Index of the last full output page's first row.
 */
static size_t cmd_tail_top(CmdCtx *c)
{
	if (c->n_off <= (size_t)c->oh)
		return 0;
	return c->n_off - (size_t)c->oh;
}

/*
 * Function: Draw the command-window status line.
 * Parameters:
 *   c (CmdCtx *): Command-window state.
 *   running_text (const char *): Running label, or NULL for final status.
 * Return (void): No return value.
 */
static void cmd_status(CmdCtx *c, const char *running_text)
{
	int w, h;
	char buf[168];
	time_t now = time(NULL);

	getmaxyx(c->win, h, w);
	(void)h;
	wmove(c->win, c->oh + 5, 1);
	wclrtoeol(c->win);
	mvwaddch(c->win, c->oh + 5, w - 1, ACS_VLINE);
	if (running_text)
		snprintf(buf, sizeof(buf), "%s (%lds)  ESC aborts",
			 running_text, (long)(now - c->start));
	else if (c->status == 0)
		snprintf(buf, sizeof(buf),
			 "finished OK - press any key");
	else
		snprintf(buf, sizeof(buf),
			 "exit status %d - press any key", c->status);
	wattron(c->win, running_text ? CP(CP_SEL) : CP(CP_DLG) | A_BOLD);
	put_line(c->win, c->oh + 5, 2, w - 4, buf);
	wattroff(c->win, running_text ? CP(CP_SEL) : CP(CP_DLG) | A_BOLD);
	c->last_sec = now;
}

/* The row between the log and the status line: a progress bar when the
 * percentage is known, an animated spinner plus "working..." while the
 * process runs without one, and nothing once it failed without one. */
/*
 * Function: Draw the command progress bar or spinner row.
 * Parameters:
 *   c (CmdCtx *): Command-window state.
 * Return (void): No return value.
 */
static void cmd_bar(CmdCtx *c)
{
	WINDOW *win = c->win;
	int w, h;

	getmaxyx(win, h, w);
	(void)h;
	wmove(win, c->oh + 3, 1);
	wclrtoeol(win);
	mvwaddch(win, c->oh + 3, w - 1, ACS_VLINE);

	if (c->pct >= 0) {
		ui_draw_progress(win, c->oh + 3, 2, w - 4, c->pct);
		return;
	}
	if (c->status != -1)
		return;
	wattron(win, CP(CP_DLG));
	ui_draw_spinner(win, c->oh + 3, 2, mono_ms() - c->t0);
	mvwaddnstr(win, c->oh + 3, 4, "working...", (size_t)(w - 6));
	wattroff(win, CP(CP_DLG));
}

/*
 * Function: Redraw command output, scrollbar, progress, and status.
 * Parameters:
 *   c (CmdCtx *): Command-window state to render.
 * Return (void): No return value.
 */
static void cmd_redraw(CmdCtx *c)
{
	WINDOW *win = c->win;
	int w, h, i;

	getmaxyx(win, h, w);
	(void)h;
	if (c->at_tail)
		c->top = cmd_tail_top(c);
	if (c->n_off > (size_t)c->oh && c->top > cmd_tail_top(c))
		c->top = cmd_tail_top(c);

	for (i = 0; i < c->oh; i++) {
		size_t idx = c->top + (size_t)i;
		int y = 2 + i;

		wmove(win, y, 1);
		wclrtoeol(win);
		mvwaddch(win, y, w - 1, ACS_VLINE);
		if (idx < c->n_off && c->out.data) {
			size_t start = c->off[idx];
			size_t end = (idx + 1 < c->n_off) ?
					    c->off[idx + 1] - 1 :
					    c->out.len - 1;

			if (end > start)
				put_span(win, y, 2, w - 4,
					 c->out.data + start, end - start);
		}
	}
	/* scroll indicator */
	if (c->n_off > (size_t)c->oh) {
		size_t span = c->n_off - (size_t)c->oh;
		int by = 2 + (int)(span ? (c->oh - 1) * c->top / span : 0);

		wattron(win, A_REVERSE);
		mvwaddch(win, by, w - 1, ACS_CKBOARD);
		wattroff(win, A_REVERSE);
	}
	cmd_bar(c);
	wrefresh(win);
}

/*
 * Function: Apply wheel movement or a scrollbar click to the live command log.
 * Parameters:
 *   c (CmdCtx *): Command-window state to update.
 *   ev (MEVENT *): Screen-relative mouse event to apply.
 *   left_click (int): Non-zero when ev represents a left click.
 * Return (int): 1 when the output viewport changed, otherwise 0.
 */
static int cmd_mouse_scroll(CmdCtx *c, const MEVENT *ev, int left_click)
{
	int my, mx, wheel;

	if (!mouse_in_window(c->win, ev, &my, &mx))
		return 0;
	wheel = mouse_wheel_dir(ev);
	if (wheel && my >= 2 && my < 2 + c->oh) {
		size_t step = (size_t)(c->oh < 3 ? c->oh : 3);
		size_t tail = cmd_tail_top(c);

		if (wheel < 0)
			c->top = c->top > step ? c->top - step : 0;
		else if (c->top < tail && step < tail - c->top)
			c->top += step;
		else
			c->top = tail;
		c->at_tail = c->top >= tail;
		return 1;
	}
	if (left_click && mx == getmaxx(c->win) - 1 &&
	    my >= 2 && my < 2 + c->oh && c->n_off > (size_t)c->oh) {
		size_t span = c->n_off - (size_t)c->oh;

		if (c->oh > 1)
			c->top = span * (size_t)(my - 2) /
				 (size_t)(c->oh - 1);
		else
			c->top = 0;
		c->at_tail = c->top >= cmd_tail_top(c);
		return 1;
	}
	return 0;
}

/* Progress-only lines, never log content: "Progress: [ 42%]" comes
 * from apt's --show-progress option, "(Reading database ... 42%)"
 * straight from dpkg, and a bare "42%" counts too (the child runs
 * with LC_ALL=C, so the prefixes are stable).  The final dpkg line
 * "(Reading database ... 300840 files ...)" has no '%' and stays in
 * the log. */
/*
 * Function: Parse a progress-only line emitted by apt or dpkg.
 * Parameters:
 *   line (const char *): Child output line to inspect.
 *   pct (int *): Receives the parsed percentage when recognized.
 * Return (int): Non-zero when line is a progress line.
 */
static int cmd_parse_pct(const char *line, int *pct)
{
	const char *s = line;
	char *end;
	long v;

	if (strncmp(s, "Progress: [", 11) == 0)
		s += 11;
	else if (strncmp(s, "(Reading database ...", 21) == 0)
		s += 21;
	else {
		while (*s == ' ')
			s++;
		if (*s < '0' || *s > '9')
			return 0;
	}
	v = strtol(s, &end, 10);
	if (end == s || *end != '%')
		return 0;
	end++;
	while (*end == ' ' || *end == ']' || *end == ')')
		end++;
	if (*end)
		return 0;
	if (v < 0)
		v = 0;
	if (v > 100)
		v = 100;
	*pct = (int)v;
	return 1;
}

/*
 * Function: Receive one child output line for the command window.
 * Parameters:
 *   ud (void *): Cast to CmdCtx *, supplied by proc_run().
 *   line (const char *): Borrowed NUL-terminated output line.
 *   is_err (int): Non-zero when the line came from standard error.
 * Return (void): No return value.
 */
static void cmd_line(void *ud, const char *line, int is_err)
{
	CmdCtx *c = ud;
	size_t start = c->out.len;
	int pct;

	(void)is_err;
	if (cmd_parse_pct(line, &pct)) {
		c->pct = pct;
		cmd_redraw(c); /* the bar follows the unpacking */
		return;
	}
	if (c->n_off + 1 > c->cap_off) {
		c->cap_off = c->cap_off ? c->cap_off * 2 : 256;
		c->off = xrealloc(c->off, c->cap_off * sizeof(*c->off));
	}
	c->off[c->n_off++] = start;
	buf_puts(&c->out, line);
	buf_putc(&c->out, '\n');
	cmd_redraw(c);
}

/*
 * Function: Process terminal input and redraw while a child is running.
 * Parameters:
 *   ud (void *): Cast to CmdCtx *, supplied by proc_run().
 * Return (int): Non-zero to request cancellation of the child.
 */
static int cmd_tick(void *ud)
{
	CmdCtx *c = ud;
	int moved = 0;

	for (;;) {
		int ch = wgetch(stdscr);

		if (ch == ERR)
			break;
		switch (ch) {
		case KEY_MOUSE: {
			MEVENT ev;

			if (getmouse(&ev) == OK) {
				int left = ui_mouse_clicked(&ev, 1);

				if (cmd_mouse_scroll(c, &ev, left))
					moved = 1;
			}
			break;
		}
		case 27:
		case KEY_F(10):
		case 'q':
			return 1;
		case KEY_UP:
			c->at_tail = 0;
			if (c->top)
				c->top--;
			moved = 1;
			break;
		case KEY_DOWN:
			if (c->top + (size_t)c->oh < c->n_off) {
				c->top++;
				c->at_tail = 0;
			}
			moved = 1;
			break;
		case KEY_PPAGE:
			c->top = c->top > (size_t)c->oh ?
					 c->top - (size_t)c->oh :
					 0;
			c->at_tail = 0;
			moved = 1;
			break;
		case KEY_NPAGE:
			c->top += (size_t)c->oh;
			if (c->top >= cmd_tail_top(c)) {
				c->top = cmd_tail_top(c);
				c->at_tail = 1;
			}
			moved = 1;
			break;
		case KEY_HOME:
			c->top = 0;
			c->at_tail = 0;
			moved = 1;
			break;
		case KEY_END:
			c->top = cmd_tail_top(c);
			c->at_tail = 1;
			moved = 1;
			break;
		default:
			break;
		}
	}
	if (time(NULL) != c->last_sec && c->status == -1)
		cmd_status(c, "running");
	if (moved)
		cmd_redraw(c);
	else {
		cmd_bar(c); /* keeps the spinner animation alive */
		wrefresh(c->win);
	}
	return 0;
}

/*
 * Function: Run a command while displaying its live output and progress.
 * Parameters:
 *   title (const char *): Command-window title.
 *   argv (char *const *): NULL-terminated command argument vector.
 *   stdin_data (const char *): Optional NUL-terminated input for the child.
 * Return (int): Child exit status, or a negative value on failure.
 */
int dlg_command(const char *title, char *const argv[],
		const char *stdin_data)
{
	CmdCtx c;
	int w = COLS - 6;
	int h = LINES - 6;
	int i;

	if (w > 90)
		w = 90;
	if (w < 30)
		w = COLS;
	if (h > 26)
		h = 26;
	if (h < 8)
		h = LINES;

	memset(&c, 0, sizeof(c));
	c.status = -1;
	c.pct = -1;
	c.at_tail = 1;
	c.start = time(NULL);
	c.t0 = mono_ms();
	c.last_sec = 0;
	buf_init(&c.out);
	c.top = 0;

	c.win = mkwin_centered(h, w, NULL, NULL);
	if (!c.win) {
		buf_free(&c.out);
		return -1;
	}
	/* rows: 1 pad, log 2..oh+1, pad, bar oh+3, pad, status oh+5 */
	c.oh = h - 8;
	if (c.oh < 1)
		c.oh = 1;

	box(c.win, 0, 0);
	wattron(c.win, CP(CP_SEL) | A_BOLD);
	mvwprintw(c.win, 0, 2, " %s ", title ? title : "Command");
	wattroff(c.win, CP(CP_SEL) | A_BOLD);
	{
		char right[96];
		Buf cmd;
		int nshown = 0;

		buf_init(&cmd);
		for (i = 0; argv && argv[i]; i++) {
			if (i == 4) {
				buf_puts(&cmd, " ...");
				break;
			}
			if (i)
				buf_putc(&cmd, ' ');
			buf_puts(&cmd, argv[i]);
			nshown++;
		}
		(void)nshown;
		buf_putc(&cmd, ' ');
		fit_into(right, sizeof(right), cmd.data ? cmd.data : "");
		buf_free(&cmd);
		wattron(c.win, CP(CP_DLG));
		mvwaddnstr(c.win, 0,
			   w - 2 - (int)strlen(right) > 1 ?
				   w - 2 - (int)strlen(right) :
				   1,
			   right, (size_t)(w - 3));
		wattroff(c.win, CP(CP_DLG));
	}
	wtimeout(stdscr, 0); /* non blocking: proc_run paces the ticks */
	cmd_status(&c, "running");
	cmd_redraw(&c);

	c.status = proc_run(argv, stdin_data, cmd_line, cmd_tick, &c);

	wtimeout(stdscr, -1);
	if (c.status == 0)
		c.pct = 100; /* finished: the bar is complete */
	cmd_redraw(&c);
	cmd_status(&c, NULL);
	wrefresh(c.win);

	for (;;) {
		int key = wgetch(stdscr);
		int moved = 0;

		switch (key) {
		case KEY_MOUSE: {
			MEVENT ev;

			if (getmouse(&ev) == OK) {
				int left = ui_mouse_clicked(&ev, 1);

				if (cmd_mouse_scroll(&c, &ev, left))
					moved = 1;
			}
			break;
		}
		case KEY_UP:
			if (c.top) {
				c.top--;
				moved = 1;
			}
			break;
		case KEY_DOWN:
			if (c.top + (size_t)c.oh < c.n_off) {
				c.top++;
				moved = 1;
			}
			break;
		case KEY_PPAGE:
			c.top = c.top > (size_t)c.oh ? c.top - (size_t)c.oh : 0;
			moved = 1;
			break;
		case KEY_NPAGE:
			c.top += (size_t)c.oh;
			if (c.top > cmd_tail_top(&c))
				c.top = cmd_tail_top(&c);
			moved = 1;
			break;
		case KEY_HOME:
			c.top = 0;
			moved = 1;
			break;
		case KEY_END:
			c.top = cmd_tail_top(&c);
			moved = 1;
			break;
		case 27:
		case '\n':
		case KEY_ENTER:
		case KEY_F(10):
		case 'q':
		case ' ':
			goto done;
		default:
			if (moved)
				break;
			/* any other key also closes the finished window */
			goto done;
		}
		if (moved)
			cmd_redraw(&c);
	}

done:
	wtimeout(stdscr, 1000);
	close_win(c.win);
	free(c.off);
	buf_free(&c.out);
	return c.status;
}

/* ------------------------------------------------------------------ *
 * Help                                                                 *
 * ------------------------------------------------------------------ */

/*
 * Function: Display the built-in keyboard and mouse help.
 * Parameters: None.
 * Return (void): No return value.
 */
void dlg_help(void)
{
	static const char help_text[] =
"PANEL NAVIGATION\n"
"  Up/Down, PgUp/PgDn, Home/End  move the cursor\n"
"  Tab or left click             switch between the two panels\n"
"  a..z, Backspace               incremental search inside a panel\n"
"  Esc                           leave the incremental search\n"
"  Enter, Right, Left            expand/collapse the dependency tree\n"
"  F3                            package information\n"
"  F4                            list of files of the package\n"
"  F7                            find a package by substring\n"
"\n"
"MOUSE\n"
"  left click                    select a package and activate its panel\n"
"  double click                  expand/collapse the dependency tree\n"
"  wheel                         scroll panels, viewers and command output\n"
"  click menus, function keys, dialog buttons and scrollbar tracks\n"
"\n"
"MARKING AND ACTIONS\n"
"  Insert                        toggle the mark of the current package\n"
"  +                             mark packages matching a glob pattern\n"
"  -                             clear all marks in the panel\n"
"  F5                            install (right panel) / reinstall\n"
"  F6                            upgrade the selected or marked packages\n"
"  F8 or Del                     remove the selected or marked packages\n"
"  Ctrl+R                        re-read the package database\n"
"\n"
"GLOBAL\n"
"  F1                            this help\n"
"  F2 or F9                      drop down menu\n"
"  F10                           quit\n"
"\n"
"KEY COLOURS\n"
"  white on blue   ordinary package\n"
"  black on cyan   cursor line\n"
"  yellow          marked package\n"
"  green           upgrade available (left panel)\n"
"  cyan            installed dependency inside a tree\n"
"\n"
"DEPENDENCY TREE\n"
"  [+] / [-]       the package has Depends/PreDepends; Enter or a\n"
"                  double click expands/collapses, Right/Left do too\n"
"  a child row is cyan when it is installed\n"
"  left panel: installed - right panel: everything else\n"
"\n"
"PROGRESS\n"
"  percentages from dpkg or apt (Progress:, Reading database ...)\n"
"  draw the bar and stay out of the log\n"
"  without one an animated spinner appears instead\n"
"\n"
"SCROLLBAR\n"
"  the right edge of each panel frame carries a yellow thumb that\n"
"  shows the position of the visible part of the list\n";

	dlg_text("Help", help_text);
}
