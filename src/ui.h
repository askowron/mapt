/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 APPIT Adam Skowroński
 */
#ifndef MAPT_UI_H
#define MAPT_UI_H

/* Pick the wide-character flavour of ncurses when its headers are split
 * into a subdirectory (Debian/Ubuntu: libncurses-dev). */
#if defined(__has_include)
#if __has_include(<ncursesw/ncurses.h>)
#include <ncursesw/ncurses.h>
#elif __has_include(<ncurses.h>)
#include <ncurses.h>
#else
#include <curses.h>
#endif
#else
#include <ncurses.h>
#endif

typedef struct {
	int x, y, w, h;
} Rect;

/* Colour pairs. */
enum {
	CP_PANEL = 1, /* white on blue: panel contents */
	CP_SEL,       /* black on cyan: cursor, headers, buttons */
	CP_MARK,      /* yellow on blue: marked packages */
	CP_DLG,       /* black on white: dialog body */
	CP_BAR,       /* white on black: menu and function key bar */
	CP_ERR,       /* white on red */
	CP_NUM,       /* yellow on black: function key numbers */
	CP_UPGR,      /* green on blue: upgradable packages */
	CP_DEP        /* cyan on blue: installed package inside a tree */
};

extern int ui_colors;

#define CP(n) (ui_colors ? COLOR_PAIR(n) : 0)

void ui_init_colors(void);

/* Decode a mouse event for button (1 left, 2 middle, 3 right) as one
 * logical click.  ev is the screen-relative ncurses event; the return
 * value is non-zero only for a newly completed click. */
int ui_mouse_clicked(const MEVENT *ev, int button);
void ui_mouse_reset(void);
/* enabled is non-zero to enable reporting and zero to disable it. */
void ui_mouse_set_enabled(int enabled);

/* Box around a rectangle with an optional title (top border, left) and an
 * optional right aligned label (top border, right). */
void ui_draw_frame(const Rect *r, const char *title, const char *right,
		   int attr);

/* Fill n cells starting at (y, x) with spaces using attr. */
void ui_fill_line(int y, int x, int n, int attr);

/* Determinate progress bar for one dialog row: "[████░░░░] 42%" with
 * U+2588 for the filled and U+2591 for the empty part.
 * pct is clamped to 0..100; the row is cleared and redrawn. */
void ui_draw_progress(WINDOW *win, int y, int x, int w, int pct);

/* One cell braille spinner frame, animated in ~90ms steps of t_ms.
 * Draw it before the text of a task when no percentage is known. */
void ui_draw_spinner(WINDOW *win, int y, int x, long t_ms);

/* One cell drop shadow along the right and bottom edge of a popup
 * window: black background cells painted on stdscr outside the frame.
 * Call it right after creating the window; the full redraw after the
 * dialog closes clears it. */
void ui_shadow(WINDOW *win);

/* Centre of a rectangle for a text of the given width. */
int ui_center_x(const Rect *r, int width);

#endif
