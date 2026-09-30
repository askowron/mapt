/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 APPIT Adam Skowroński
 */
#include "menu.h"
#include "dialog.h"
#include "ui.h"
#include "util.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

static const MenuEntry file_items[] = {
	{ "Info                    F3", CMD_INFO },
	{ "List of files           F4", CMD_CONTENTS },
	{ "Find...                 F7", CMD_FIND },
	{ "---", CMD_SEP },
	{ "Refresh package data Ctrl+R", CMD_RELOAD },
	{ "Update package lists     ", CMD_UPDATE },
	{ "---", CMD_SEP },
	{ "Quit                    F10", CMD_QUIT },
	{ NULL, 0 }
};

static const MenuEntry mark_items[] = {
	{ "Toggle mark           Insert", CMD_MARK_TOGGLE },
	{ "Mark by pattern...        +", CMD_MARK_PATTERN },
	{ "Clear marks in panel      -", CMD_MARK_CLEAR },
	{ NULL, 0 }
};

static const MenuEntry command_items[] = {
	{ "Install / upgrade       F5", CMD_INSTALL },
	{ "Reinstall selected        ", CMD_REINSTALL },
	{ "Upgrade selected       F6", CMD_UPGRADE },
	{ "Upgrade all packages      ", CMD_UPGRADE_ALL },
	{ "Update and upgrade all    ", CMD_UPDATE_UPGRADE },
	{ "Dist-upgrade all          ", CMD_DIST_UPGRADE },
	{ "---", CMD_SEP },
	{ "Remove selected        F8", CMD_REMOVE },
	{ "Purge selected            ", CMD_PURGE },
	{ "Autoremove packages       ", CMD_AUTOREMOVE },
	{ NULL, 0 }
};

static const MenuEntry options_items[] = {
	{ "---", CMD_SEP },
	{ "Sort by name             ", CMD_SORT_NAME },
	{ "Sort by version          ", CMD_SORT_VERSION },
	{ "Sort by size             ", CMD_SORT_SIZE },
	{ "Reverse sort order       ", CMD_SORT_REVERSE },
	{ "---", CMD_SEP },
	{ "Left panel content       ", CMD_PANEL_MODE },
	{ NULL, 0 }
};

static const MenuEntry help_items[] = {
	{ "Help                   F1", CMD_HELP },
	{ "About                     ", CMD_ABOUT },
	{ NULL, 0 }
};

static const Menu g_menus[] = {
	{ "File", file_items },
	{ "Mark", mark_items },
	{ "Command", command_items },
	{ "Options", options_items },
	{ "Help", help_items }
};

#define N_MENUS ((int)(sizeof(g_menus) / sizeof(g_menus[0])))

/*
 * Function: Build the right-aligned status text and return its screen width.
 * Parameters:
 *   buf (char *): Destination buffer for the formatted text.
 *   cap (size_t): Capacity of buf in bytes, including the terminating NUL.
 * Return (int): Length of the formatted string in bytes.
 */
static int menu_right_text(char *buf, size_t cap)
{
	char clk[16];

	clock_str(clk, sizeof(clk));
	snprintf(buf, cap, "%s %s  %s ", MAPT_NAME, MAPT_VERSION, clk);
	return (int)strlen(buf);
}

/*
 * Function: Calculate the screen column of a menu title.
 * Parameters:
 *   idx (int): Zero-based top-level menu index.
 * Return (int): Screen column where that title's slot begins.
 */
static int menu_x(int idx)
{
	int x = 0, i;

	for (i = 0; i < idx && i < N_MENUS; i++)
		x += (int)strlen(g_menus[i].title) + 2;
	return x;
}

/*
 * Function: Find the visible top-level menu title under a screen column.
 * Parameters:
 *   x (int): Zero-based screen column in the menu bar.
 * Return (int): Menu index, or -1 when the column is not a visible title.
 */
int menubar_hit(int x)
{
	char right[80];
	int right_x = COLS - menu_right_text(right, sizeof(right));
	int start = 0, i;

	for (i = 0; i < N_MENUS; i++) {
		int len = (int)strlen(g_menus[i].title);

		if (start + len + 1 > right_x)
			break;
		if (x >= start && x < start + len + 2 && x < right_x)
			return i;
		start += len + 2;
	}
	return -1;
}

/*
 * Function: Draw the top menu bar and its right-aligned status text.
 * Parameters:
 *   open_idx (int): Highlighted menu index, or -1 when no menu is open.
 * Return (void): No return value.
 */
void menubar_draw(int open_idx)
{
	int x = 0, i;
	char right[80];
	int rl = menu_right_text(right, sizeof(right));
	int right_x = COLS - rl;

	ui_fill_line(0, 0, COLS, CP(CP_BAR));

	for (i = 0; i < N_MENUS; i++) {
		int len = (int)strlen(g_menus[i].title);
		int a = (i == open_idx) ? CP(CP_SEL) :
					  (CP(CP_BAR) | A_BOLD);

		if (x + len + 1 > right_x)
			break;
		attron(a);
		mvaddnstr(0, x + 1, g_menus[i].title, len);
		attroff(a);
		x += len + 2;
	}

	if (rl < COLS) {
		attron(CP(CP_BAR) | A_BOLD);
		mvaddnstr(0, COLS - rl, right, rl);
		attroff(CP(CP_BAR) | A_BOLD);
	}
}

/*
 * Function: Run a menu loop, optionally opening at the title selected by the mouse.
 * Parameters:
 *   st (MenuState *): Current panel and option state used to draw markers.
 *   initial (int): Top-level menu index to open first.
 * Return (int): Selected Command, or CMD_NONE when the menu is cancelled.
 */
int menubar_run(const MenuState *st, int initial)
{
	int mi = initial;

	if (!st)
		return CMD_NONE;
	if (mi < 0 || mi >= N_MENUS)
		mi = 0;

	for (;;) {
		const Menu *m = &g_menus[mi];
		const char *labels[32];
		char buf[32][72];
		int cmds[32];
		int n = 0, i, r, top_x = -1;

		for (i = 0; m->items[i].label && n < 31; i++) {
			const char *lab = m->items[i].label;
			int cmd = m->items[i].cmd;

			if (strcmp(lab, "---") == 0) {
				snprintf(buf[n], sizeof(buf[n]), "---");
			} else {
				char mark = ' ';

				switch (cmd) {
				case CMD_SORT_NAME:
					mark = st->sort[st->active] ==
							       SORT_NAME &&
						       !st->desc[st->active] ?
						       '*' :
						       (st->sort[st->active] ==
									SORT_NAME ?
									'-' :
								' ');
					break;
				case CMD_SORT_VERSION:
					mark = st->sort[st->active] ==
						       SORT_VERSION ?
						       (st->desc[st->active] ?
									'-' :
								'*') :
						       ' ';
					break;
				case CMD_SORT_SIZE:
					mark = st->sort[st->active] ==
						       SORT_SIZE ?
						       (st->desc[st->active] ?
									'-' :
								'*') :
						       ' ';
					break;
				case CMD_SORT_REVERSE:
					mark = st->desc[st->active] ? '*' :
								       ' ';
					break;
				case CMD_PANEL_MODE:
					snprintf(buf[n], sizeof(buf[n]),
						 "%c Left panel: %s",
						 ' ',
						 st->left_only_upgradeable ?
							 "upgradable only" :
							 "all installed");
					labels[n] = buf[n];
					cmds[n] = cmd;
					n++;
					continue;
				default:
					break;
				}
				snprintf(buf[n], sizeof(buf[n]), "%c %s",
					 mark, lab);
			}
			labels[n] = buf[n];
			cmds[n] = cmd;
			n++;
		}
		labels[n] = NULL;

		menubar_draw(mi);
		refresh();

		r = dlg_menu(labels, n, 0, 1, menu_x(mi), &top_x);

		if (r == DLG_MENU_TOP) {
			int target = menubar_hit(top_x);

			if (target < 0 || target == mi)
				return CMD_NONE;
			mi = target;
			continue;
		}
		if (r == DLG_MENU_LEFT) {
			mi = (mi + N_MENUS - 1) % N_MENUS;
			continue;
		}
		if (r == DLG_MENU_RIGHT) {
			mi = (mi + 1) % N_MENUS;
			continue;
		}
		if (r == DLG_MENU_CANCEL || r < 0 || r >= n)
			return CMD_NONE;
		return cmds[r];
	}
}
