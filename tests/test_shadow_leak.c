/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 APPIT Adam Skowroński
 */
/* Reproduces the stale drop-shadow left on stdscr when one popup opens on
 * top of another without an intervening full redraw:
 * menu dropdown (with shadow) -> deleted -> centred dialog (with shadow).
 * The menu shadow cells must be gone once the dialog is up. */
#include "ui.h"

#include <stdio.h>
#include <string.h>

#define FRAME ' '

/* A leftover shadow cell is a blank painted in the menu-bar colour pair.
 * A chtype carries attributes too, so compare the character only. */
static int is_shadow_cell(chtype c)
{
	if ((c & A_CHARTEXT) != FRAME)
		return 0;
	if (!ui_colors)
		return 1;
	return PAIR_NUMBER(c) == CP_BAR;
}

/* Count shadow cells left on stdscr in the band that framed a window of
 * (h, w) at (y, x): the right column and the bottom row ui_shadow paints. */
static int shadow_leftovers(int h, int w, int y, int x)
{
	int i, bad = 0;

	for (i = 1; i < h && y + i < LINES; i++)
		if (is_shadow_cell(mvwinch(stdscr, y + i, x + w)))
			bad++;
	for (i = 1; i <= w && x + i < COLS; i++)
		if (is_shadow_cell(mvwinch(stdscr, y + h, x + i)))
			bad++;
	return bad;
}

int main(void)
{
	int menu_h = 12, menu_w = 32, menu_y = 1, menu_x = 1;
	int dlg_h = 16, dlg_w = 40, dlg_y, dlg_x;
	WINDOW *menu, *dlg;
	int stale_before, stale_after, restored;

	initscr();
	cbreak();
	noecho();
	curs_set(0);
	ui_init_colors();

	/* Background screen the way the main loop paints it: panel blue
	 * cells everywhere, so a restored shadow band is recognisable. */
	erase();
	attron(CP(CP_PANEL));
	for (int r = 0; r < LINES; r++)
		mvhline(r, 0, '.', COLS);
	attroff(CP(CP_PANEL));
	refresh();

	/* Popup 1: the dropdown menu, shadow painted, then destroyed. */
	menu = newwin(menu_h, menu_w, menu_y, menu_x);
	wbkgd(menu, CP(CP_DLG));
	ui_shadow(menu);
	box(menu, 0, 0);
	wrefresh(menu);

	stale_before = shadow_leftovers(menu_h, menu_w, menu_y, menu_x);
	if (stale_before == 0) {
		endwin();
		printf("FAIL: ui_shadow painted nothing\n");
		return 1;
	}

	ui_unshadow(menu);
	delwin(menu);
	refresh();

	/* Popup 2 opens immediately, exactly like File > Info does. */
	dlg_y = (LINES - dlg_h) / 2;
	dlg_x = (COLS - dlg_w) / 2;
	dlg = newwin(dlg_h, dlg_w, dlg_y, dlg_x);
	wbkgd(dlg, CP(CP_DLG));
	ui_shadow(dlg);
	box(dlg, 0, 0);
	wrefresh(dlg);

	stale_after = shadow_leftovers(menu_h, menu_w, menu_y, menu_x);

	/* The restored band must be the panel background, not blanks. */
	restored = 0;
	for (int i = 1; i <= menu_w && menu_x + i < COLS; i++) {
		chtype c = mvwinch(stdscr, menu_y + menu_h, menu_x + i);

		if ((c & A_CHARTEXT) == '.' &&
		    PAIR_NUMBER(c) == CP_PANEL)
			restored++;
	}

	ui_unshadow(dlg);
	delwin(dlg);
	endwin();

	printf("shadow cells painted by ui_shadow:      %d\n", stale_before);
	printf("shadow cells surviving into next dialog: %d\n", stale_after);
	printf("background cells restored by ui_unshadow: %d of %d\n",
	       restored, menu_w);
	if (stale_after > 0) {
		printf("FAIL: menu shadow survives into the next dialog\n");
		return 1;
	}
	if (restored == 0) {
		printf("FAIL: ui_unshadow left blanks instead of the panel\n");
		return 1;
	}
	printf("all shadow leak tests passed\n");
	return 0;
}
