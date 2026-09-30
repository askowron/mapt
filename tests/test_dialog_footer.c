/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 APPIT Adam Skowroński
 */
/* Checks the dialog footer: the key hint and the bright separator rule
 * drawn one row above it.  dialog.c is included directly because the
 * footer helper is static. */
#include "../src/dialog.c"

#include <stdio.h>
#include <string.h>

static int failures;

/*
 * Function: Test one failure condition and keep going.
 * Parameters:
 *   cond (int): Non-zero when the condition holds.
 *   what (const char *): Description of the condition.
 * Return (void): No return value.
 */
static void check(int cond, const char *what)
{
	if (cond) {
		printf("  ok    %s\n", what);
		return;
	}
	printf("  FAIL  %s\n", what);
	failures++;
}

/*
 * Function: Report the character and colour pair of one window cell.
 * Parameters:
 *   win (WINDOW *): Window to read.
 *   y (int): Row.
 *   x (int): Column.
 * Return (chtype): Cell contents, suitable for printing.
 */
static chtype cell(WINDOW *win, int y, int x)
{
	return mvwinch(win, y, x);
}

/*
 * Function: Check the footer of a dialog window of the given size.
 * Parameters:
 *   h (int): Window height.
 *   w (int): Window width.
 *   hint (const char *): Hint text handed to draw_footer().
 * Return (void): No return value.
 */
static void footer_case(int h, int w, const char *hint)
{
	WINDOW *win = newwin(h, w, 2, 3);
	chtype rule, first;
	int i, ok = 1;

	if (!win) {
		printf("  FAIL  newwin(%d, %d)\n", h, w);
		failures++;
		return;
	}
	wbkgd(win, CP(CP_DLG));
	box(win, 0, 0);
	draw_footer(win, hint);
	wrefresh(win);

	rule = cell(win, h - 4, 2);
	first = cell(win, h - 3, 2);

	/* A_CHARTEXT masks A_ALTCHARSET out, so test the two separately. */
	check((rule & A_CHARTEXT) == (unsigned char)'q',
	      "separator row uses the horizontal line glyph");
	check((rule & A_ALTCHARSET) != 0,
	      "separator row is drawn in the alternate character set");
	check(PAIR_NUMBER(rule) == CP_SEL,
	      "separator row is painted in the cyan selection colour");
	check((rule & A_BOLD) != 0, "separator row is bold");

	/* The rule must span the window between the two border columns. */
	for (i = 1; ok && i < w - 1; i++) {
		chtype c = cell(win, h - 4, i);

		if (PAIR_NUMBER(c) != CP_SEL || !(c & A_ALTCHARSET))
			ok = 0;
	}
	check(ok, "separator spans the full window width");

	check((first & A_CHARTEXT) == (unsigned char)hint[0],
	      "hint starts on the row below the separator");

	/* Nothing of the body may be overwritten: the row above the rule
	 * has to stay free of the hint text. */
	check((cell(win, h - 4, 2) & A_CHARTEXT) != (unsigned char)hint[0],
	      "the rule row is not the hint row");

	delwin(win);
}

int main(void)
{
	initscr();
	cbreak();
	noecho();
	curs_set(0);
	ui_init_colors();

	printf("dlg_text footer (h=22)\n");
	footer_case(22, 40,
		    "arrows/wheel/PgUp/PgDn scroll   Enter/Esc/q close");
	printf("dlg_input footer (h=9)\n");
	footer_case(9, 40, "[Enter] OK   [Esc] Cancel");

	endwin();
	if (failures) {
		printf("%d check(s) failed\n", failures);
		return 1;
	}
	printf("all dialog footer tests passed\n");
	return 0;
}
