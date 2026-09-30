/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 APPIT Adam Skowroński
 */
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"

int ui_colors = 0;

/* Cells the shadow of the currently open popup covers.  Closing one popup
 * usually leads straight into the next one (File > Info, Find... then an
 * alert), and the main loop redraws the screen only after the last one
 * closes.  Saving the covered cells lets ui_unshadow put the background
 * back instead of leaving black cells behind the next dialog. */
static cchar_t *g_shadow_cells;
static int g_shadow_y, g_shadow_x, g_shadow_h, g_shadow_w;

/* Mouse reports are not identical across terminals.  Some drivers expose
 * a physical click as PRESSED followed by CLICKED, while others emit a
 * double-click event directly.  Keep the small state machine here so
 * every dialog applies the same click semantics. */
static int mouse_down[4];
static int mouse_suppress_click[4];
static int mouse_ignore_release[4];
static int mouse_last_x[4], mouse_last_y[4];
static int mouse_have_last[4];
static long mouse_suppress_until[4];

/*
 * Function: Clear the temporary mouse-button state.
 * Parameters: None.
 * Return (void): No return value.
 */
void ui_mouse_reset(void)
{
	memset(mouse_down, 0, sizeof(mouse_down));
	memset(mouse_suppress_click, 0, sizeof(mouse_suppress_click));
	memset(mouse_ignore_release, 0, sizeof(mouse_ignore_release));
	memset(mouse_last_x, 0, sizeof(mouse_last_x));
	memset(mouse_last_y, 0, sizeof(mouse_last_y));
	memset(mouse_have_last, 0, sizeof(mouse_have_last));
	memset(mouse_suppress_until, 0, sizeof(mouse_suppress_until));
	for (int i = 0; i < 4; i++)
		mouse_ignore_release[i] = 1;
}

/*
 * Function: Temporarily disable or enable terminal mouse reporting.
 * Parameters:
 *   enabled (int): Non-zero enables reporting; zero disables it.
 * Return (void): No return value.
 */
void ui_mouse_set_enabled(int enabled)
{
	mousemask(enabled ? ALL_MOUSE_EVENTS : 0, NULL);
	flushinp();
	ui_mouse_reset();
}

/*
 * Function: Classify one mouse event as a logical button click.
 * Parameters:
 *   ev (MEVENT *): Screen-relative ncurses mouse event to inspect.
 *   button (int): Button number: 1 left, 2 middle, or 3 right.
 * Return (int): 1 for one logical click, otherwise 0.
 */
int ui_mouse_clicked(const MEVENT *ev, int button)
{
	mmask_t pressed = 0, released = 0, clicked = 0;
	int idx = button;

	if (button < 1 || button > 3)
		return 0;
	switch (button) {
	case 1:
		pressed = BUTTON1_PRESSED;
		released = BUTTON1_RELEASED;
		clicked = BUTTON1_CLICKED;
		break;
	case 2:
		pressed = BUTTON2_PRESSED;
		released = BUTTON2_RELEASED;
		clicked = BUTTON2_CLICKED;
		break;
	case 3:
		pressed = BUTTON3_PRESSED;
		released = BUTTON3_RELEASED;
		clicked = BUTTON3_CLICKED;
		break;
	}
	/* A double-click is already a complete logical action. */
	if (button == 1 && (ev->bstate & BUTTON1_DOUBLE_CLICKED)) {
		mouse_down[idx] = 0;
		mouse_ignore_release[idx] = 1;
		mouse_suppress_click[idx] = 0;
		mouse_last_x[idx] = ev->x;
		mouse_last_y[idx] = ev->y;
		mouse_have_last[idx] = 1;
		return 1;
	}
	if (ev->bstate & pressed) {
		mouse_down[idx] = 1;
		mouse_ignore_release[idx] = 0;
		mouse_suppress_click[idx] = 0;
		mouse_last_x[idx] = ev->x;
		mouse_last_y[idx] = ev->y;
		mouse_have_last[idx] = 1;
		return 1;
	}
	if (ev->bstate & released) {
		if (mouse_ignore_release[idx]) {
			mouse_ignore_release[idx] = 0;
			mouse_suppress_click[idx] = 1;
			mouse_suppress_until[idx] = mono_ms() + 250;
			mouse_last_x[idx] = ev->x;
			mouse_last_y[idx] = ev->y;
			mouse_have_last[idx] = 1;
			return 0;
		}
		if (mouse_down[idx]) {
			mouse_down[idx] = 0;
			mouse_ignore_release[idx] = 1;
			mouse_suppress_click[idx] = 1;
			mouse_suppress_until[idx] = mono_ms() + 250;
			mouse_last_x[idx] = ev->x;
			mouse_last_y[idx] = ev->y;
			mouse_have_last[idx] = 1;
			return 0;
		}
		return 0;
	}
	if (ev->bstate & clicked) {
		if (mouse_suppress_click[idx] &&
		    (!mouse_have_last[idx] || ev->x != mouse_last_x[idx] ||
		     ev->y != mouse_last_y[idx])) {
			mouse_suppress_click[idx] = 0;
			mouse_suppress_until[idx] = 0;
		}
		if (mouse_suppress_click[idx] &&
		    mono_ms() >= mouse_suppress_until[idx])
			mouse_suppress_click[idx] = 0;
		if (mouse_down[idx] || mouse_suppress_click[idx]) {
			mouse_down[idx] = 0;
			mouse_suppress_click[idx] = 0;
			mouse_ignore_release[idx] = 1;
			return 0;
		}
		if (mono_ms() < mouse_suppress_until[idx]) {
			mouse_ignore_release[idx] = 1;
			return 0;
		}
		mouse_ignore_release[idx] = 1;
		mouse_last_x[idx] = ev->x;
		mouse_last_y[idx] = ev->y;
		mouse_have_last[idx] = 1;
		return 1;
	}
	return 0;
}

/*
 * Function: Initialize ncurses colour pairs used by the interface.
 * Parameters: None.
 * Return (void): No return value; ui_colors records whether colours exist.
 */
void ui_init_colors(void)
{
	if (!has_colors()) {
		ui_colors = 0;
		return;
	}
	start_color();
	use_default_colors();

	init_pair(CP_PANEL, COLOR_WHITE, COLOR_BLUE);
	init_pair(CP_SEL, COLOR_BLACK, COLOR_CYAN);
	init_pair(CP_MARK, COLOR_YELLOW, COLOR_BLUE);
	init_pair(CP_DLG, COLOR_BLACK, COLOR_WHITE);
	init_pair(CP_BAR, COLOR_WHITE, COLOR_BLACK);
	init_pair(CP_ERR, COLOR_WHITE, COLOR_RED);
	init_pair(CP_NUM, COLOR_YELLOW, COLOR_BLACK);
	init_pair(CP_UPGR, COLOR_GREEN, COLOR_BLUE);
	init_pair(CP_DEP, COLOR_CYAN, COLOR_BLUE);
	ui_colors = 1;
}

/*
 * Function: Fill a horizontal run of cells with spaces.
 * Parameters:
 *   y (int): Zero-based screen row.
 *   x (int): Zero-based starting screen column.
 *   n (int): Number of cells to fill; non-positive values do nothing.
 *   attr (int): ncurses attribute used for the cells.
 * Return (void): No return value.
 */
void ui_fill_line(int y, int x, int n, int attr)
{
	int i;

	if (y < 0 || y >= LINES || n <= 0)
		return;
	attron(attr);
	for (i = 0; i < n; i++) {
		int cx = x + i;

		if (cx < 0 || cx >= COLS)
			continue;
		mvaddch(y, cx, ' ');
	}
	attroff(attr);
}

/*
 * Function: Number of screen cells a drop shadow would cover.
 * Parameters:
 *   h (int): Window height.
 *   w (int): Window width.
 *   y (int): Window top row.
 *   x (int): Window left column.
 * Return (int): Cell count of the shadow band.
 */
static int shadow_cells(int h, int w, int y, int x)
{
	int n = 0;

	if (x + w < COLS) {
		int i;

		for (i = 1; i < h && y + i < LINES; i++)
			n++;
	}
	if (y + h < LINES) {
		int i;

		for (i = 1; i <= w && x + i < COLS; i++)
			n++;
	}
	return n;
}

/*
 * Function: Paint a one-cell drop shadow to the right and below a window.
 * Parameters:
 *   win (WINDOW *): Window whose screen position and size define the shadow.
 * Return (void): No return value.
 */
void ui_shadow(WINDOW *win)
{
	int y, x, h, w, i, n;

	if (!win)
		return;
	getbegyx(win, y, x);
	getmaxyx(win, h, w);

	n = shadow_cells(h, w, y, x);
	free(g_shadow_cells);
	g_shadow_cells = n > 0 ? xcalloc((size_t)n, sizeof(*g_shadow_cells)) :
				 NULL;
	g_shadow_y = y;
	g_shadow_x = x;
	g_shadow_h = h;
	g_shadow_w = w;

	i = 0;
	if (x + w < COLS) { /* right column: one row down, to y+h-1 */
		int r;

		for (r = 1; r < h && y + r < LINES; r++, i++)
			if (g_shadow_cells)
				mvwin_wch(stdscr, y + r, x + w,
					  &g_shadow_cells[i]);
	}
	if (y + h < LINES) { /* bottom row: one column in, corner kept */
		int c;

		for (c = 1; c <= w && x + c < COLS; c++, i++)
			if (g_shadow_cells)
				mvwin_wch(stdscr, y + h, x + c,
					  &g_shadow_cells[i]);
	}

	/* CP_BAR is white on black: a space renders as a solid black
	 * cell - darker than the blue panel background behind a popup,
	 * so the window appears to hover above the panels. */
	attron(CP(CP_BAR));
	if (x + w < COLS) { /* right column: one row down, to y+h-1 */
		for (i = 1; i < h && y + i < LINES; i++)
			mvaddch(y + i, x + w, ' ');
	}
	if (y + h < LINES) { /* bottom row: one column in, corner kept */
		for (i = 1; i <= w && x + i < COLS; i++)
			mvaddch(y + h, x + i, ' ');
	}
	attroff(CP(CP_BAR));

	/* Push stdscr to the virtual screen before the first
	 * wrefresh(win): the doupdate inside it then paints the shadow
	 * and the dialog in one pass. */
	wnoutrefresh(stdscr);
}

/*
 * Function: Restore the cells a drop shadow covered and forget them.
 * Parameters:
 *   win (WINDOW *): The window whose shadow ui_shadow painted.  Its
 *     position and size must be unchanged since that call.
 * Return (void): No return value.
 */
void ui_unshadow(WINDOW *win)
{
	cchar_t *cells = g_shadow_cells;
	int y, x, h, w, i;

	g_shadow_cells = NULL;

	if (!win || !cells)
		return;
	getbegyx(win, y, x);
	getmaxyx(win, h, w);

	/* Only restore when the geometry still matches: the saved band is
	 * indexed with the loops below, so a moved window would read past
	 * the end of it.  Losing the background beats crashing. */
	if (y != g_shadow_y || x != g_shadow_x || h != g_shadow_h ||
	    w != g_shadow_w) {
		free(cells);
		return;
	}

	i = 0;
	if (x + w < COLS) {
		int r;

		for (r = 1; r < h && y + r < LINES; r++, i++)
			mvwadd_wch(stdscr, y + r, x + w, &cells[i]);
	}
	if (y + h < LINES) {
		int c;

		for (c = 1; c <= w && x + c < COLS; c++, i++)
			mvwadd_wch(stdscr, y + h, x + c, &cells[i]);
	}
	free(cells);
}

/*
 * Function: Draw a rectangular frame with optional title labels.
 * Parameters:
 *   r (Rect *): Rectangle to draw in screen coordinates.
 *   title (char *): Optional left title on the top border.
 *   right (char *): Optional right-aligned label on the top border.
 *   attr (int): ncurses attribute used for the frame and labels.
 * Return (void): No return value.
 */
void ui_draw_frame(const Rect *r, const char *title, const char *right,
		   int attr)
{
	int x = r->x, y = r->y, w = r->w, h = r->h;
	int tlen = title ? (int)strlen(title) : 0;
	int rlen = right ? (int)strlen(right) : 0;

	if (w < 2 || h < 2)
		return;

	attron(attr);
	mvaddch(y, x, ACS_ULCORNER);
	if (w > 2)
		mvhline(y, x + 1, ACS_HLINE, w - 2);
	mvaddch(y, x + w - 1, ACS_URCORNER);
	mvvline(y + 1, x, ACS_VLINE, h - 2);
	mvvline(y + 1, x + w - 1, ACS_VLINE, h - 2);
	mvaddch(y + h - 1, x, ACS_LLCORNER);
	if (w > 2)
		mvhline(y + h - 1, x + 1, ACS_HLINE, w - 2);
	mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
	attroff(attr);

	if (title && tlen > 0 && w > 6) {
		char buf[256];

		fit_into(buf, sizeof(buf) < (size_t)(w - 4) ?
				 sizeof(buf) : (size_t)(w - 4), title);
		attron(attr | A_BOLD);
		mvaddstr(y, x + 2, buf);
		attroff(attr | A_BOLD);
	}
	if (right && rlen > 0 && w > 6) {
		int rx = x + w - 1 - rlen;

		if (rx > x + 1 + tlen) {
			attron(attr);
			mvaddstr(y, rx, right);
			attroff(attr);
		}
	}
}

/*
 * Function: Calculate a centered x coordinate inside a rectangle.
 * Parameters:
 *   r (Rect *): Rectangle that contains the text.
 *   width (int): Width of the text in cells.
 * Return (int): Centered screen column, kept one cell inside the frame.
 */
int ui_center_x(const Rect *r, int width)
{
	int x = r->x + (r->w - width) / 2;

	if (x < r->x + 1)
		x = r->x + 1;
	return x;
}

/*
 * Function: Draw a determinate progress bar on one dialog row.
 * Parameters:
 *   win (WINDOW *): Target window.
 *   y (int): Target row in window coordinates.
 *   x (int): Starting column in window coordinates.
 *   w (int): Total bar width in cells.
 *   pct (int): Progress percentage; values are clamped to 0..100.
 * Return (void): No return value.
 */
void ui_draw_progress(WINDOW *win, int y, int x, int w, int pct)
{
	int bar, filled, i;
	char lab[8];

	if (w < 16)
		return;
	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;
	bar = w - 8; /* brackets, gap and the 4 character label */
	filled = pct * bar / 100;
	snprintf(lab, sizeof(lab), "%3d%%", pct);

	wattron(win, CP(CP_DLG));
	for (i = 0; i < w; i++)
		mvwaddch(win, y, x + i, ' ');
	mvwaddch(win, y, x, '[');
	mvwaddch(win, y, x + bar + 1, ']');
	wattroff(win, CP(CP_DLG));
	for (i = 0; i < bar; i++) {
		int on = i < filled;

		wattron(win, on ? CP(CP_SEL) : CP(CP_DLG));
		/* U+2588 full block, U+2591 light shade: both are one
		 * cell wide, so the layout does not move. */
		mvwaddstr(win, y, x + 1 + i,
			  on ? "\u2588" : "\u2591");
		wattroff(win, on ? CP(CP_SEL) : CP(CP_DLG));
	}
	wattron(win, CP(CP_DLG));
	mvwaddnstr(win, y, x + bar + 3, lab, 4);
	wattroff(win, CP(CP_DLG));
}

/*
 * Function: Draw one animated braille spinner frame.
 * Parameters:
 *   win (WINDOW *): Target window.
 *   y (int): Target row in window coordinates.
 *   x (int): Target column in window coordinates.
 *   t_ms (long): Elapsed animation time in milliseconds.
 * Return (void): No return value.
 */
void ui_draw_spinner(WINDOW *win, int y, int x, long t_ms)
{
	static const char *const frame[10] = {
		"\u280B", "\u2819", "\u2839", "\u2838", "\u283C",
		"\u2834", "\u2826", "\u2827", "\u2807", "\u280F"
	};

	if (t_ms < 0)
		t_ms = 0;
	mvwaddstr(win, y, x, frame[(t_ms / 90) % 10]);
}
