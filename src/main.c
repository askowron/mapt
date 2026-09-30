/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 APPIT Adam Skowroński
 */
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "apt.h"
#include "cmd.h"
#include "dialog.h"
#include "menu.h"
#include "panel.h"
#include "pkg.h"
#include "ui.h"
#include "util.h"
#include "version.h"

static PkgDB g_db;
static Panel g_panels[2];
static int g_active; /* 0 = left, 1 = right */
static Rect g_prect[2], g_srect, g_frect;
static char g_clock[16];
static WINDOW *g_load_win;
static char g_load_msg[128]; /* last step text, kept for the spinner */
static long g_load_t0, g_load_drawn;
static int g_origin_failed; /* stop asking for origins after a failure */
static int g_deps_failed;   /* stop asking for deps after a failure */

/* Manual double-click fallback for terminals that do not expose a
 * BUTTON1_DOUBLE_CLICKED event of their own. */
static int g_click_panel = -1;
static size_t g_click_index;
static long g_click_time;

/* How many rows beyond the visible ones are resolved in advance, so
 * that scrolling does not stall on every line. */
#define ORIGIN_LOOKAHEAD 100
#define MOUSE_DOUBLE_MS 400 /* maximum gap for the manual double-click fallback */

/* Keep the function-bar geometry and its mouse commands in one place. */
static const char *const g_fkey_num[10] = { "1",  "2",  "3",   "4", "5",
					    "6",  "7",  "8",   "9", "10" };
static const char *const g_fkey_label[10] = { "Help", "Menu",  "Info",
					      "List", "Inst", "Upgr",
					      "Find", "Del",  "Menu",
					      "Quit" };
static const int g_fkey_cmd[10] = {
	CMD_HELP, CMD_MENU, CMD_INFO, CMD_CONTENTS, CMD_INSTALL,
	CMD_UPGRADE, CMD_FIND, CMD_REMOVE, CMD_MENU, CMD_QUIT
};

/* ------------------------------------------------------------------ *
 * Layout                                                               *
 * ------------------------------------------------------------------ */

/*
 * Function: Recalculate screen rectangles for the current terminal size.
 * Parameters: None.
 * Return (void): No return value.
 */
static void layout(void)
{
	g_prect[0].x = 0;
	g_prect[0].y = 1;
	g_prect[0].w = COLS / 2;
	g_prect[0].h = LINES - 3;

	g_prect[1].x = COLS / 2;
	g_prect[1].y = 1;
	g_prect[1].w = COLS - COLS / 2;
	g_prect[1].h = LINES - 3;

	g_srect.x = 0;
	g_srect.y = LINES - 2;
	g_srect.w = COLS;
	g_srect.h = 1;

	g_frect.x = 0;
	g_frect.y = LINES - 1;
	g_frect.w = COLS;
	g_frect.h = 1;
}

/*
 * Function: Check whether the terminal meets the minimum layout size.
 * Parameters: None.
 * Return (int): Non-zero when the terminal is too small.
 */
static int too_small(void)
{
	return COLS < 30 || LINES < 8;
}

/*
 * Function: Draw the terminal-too-small message.
 * Parameters: None.
 * Return (void): No return value.
 */
static void draw_small(void)
{
	erase();
	attron(CP(CP_ERR) | A_BOLD);
	mvprintw(LINES / 2 - 1, 2, "Terminal too small (%dx%d).", COLS,
		 LINES);
	mvprintw(LINES / 2, 2, "Need at least 30x8. Press any key.");
	attroff(CP(CP_ERR) | A_BOLD);
	refresh();
}

/*
 * Function: Draw the status line for the active panel.
 * Parameters: None.
 * Return (void): No return value.
 */
static void draw_status(void)
{
	Panel *p = &g_panels[g_active];
	int y = g_srect.y;
	char right[48];
	char left[320];
	int rl;

	ui_fill_line(y, 0, COLS, CP(CP_BAR));

	snprintf(right, sizeof(right), "%zu/%zu ",
		 p->cur + (p->n ? 1 : 0), p->n);
	rl = (int)strlen(right);

	if (p->search_len) {
		snprintf(left, sizeof(left), "Find: %s", p->search);
		attron(CP(CP_NUM) | A_BOLD);
		mvaddnstr(y, 1, left, COLS - 2);
		attroff(CP(CP_NUM) | A_BOLD);
	} else {
		Package *pk = panel_current(p);

		if (pk) {
			char sizebuf[32], ver[64];
			size_t used;

			fit_into(ver, sizeof(ver), pk->installed ?
							   pk->installed :
						   pk->candidate ?
							   pk->candidate :
						   "-");
			if ((pk->flags & PKGF_INSTALLED) && pk->size_kib) {
				human_kib(pk->size_kib, sizebuf,
					  sizeof(sizebuf));
			} else {
				sizebuf[0] = '-';
				sizebuf[1] = '\0';
			}
			snprintf(left, sizeof(left),
				 "%s  %s  %s%s%s  %s", pk->name, ver,
				 pk->arch ? pk->arch : "",
				 pk->arch ? "  " : "",
				 sizebuf,
				 (pk->flags & PKGF_MARKED) ? "  MARKED" : "");

			/* The origin does not always fit into a panel
			 * column; show it here when there is room. */
			used = strlen(left);
			if (pk->repo && COLS - 2 - rl > (int)used + 2) {
				char org[192];
				int room = COLS - 2 - rl - (int)used - 2;

				fit_into(org, (size_t)room + 1, pk->repo);
				left[used] = ' ';
				left[used + 1] = ' ';
				memcpy(left + used + 2, org, strlen(org) + 1);
			}
		} else {
			snprintf(left, sizeof(left), "no package");
		}
		attron(CP(CP_BAR) | A_BOLD);
		mvaddnstr(y, 1, left, COLS - 2);
		attroff(CP(CP_BAR) | A_BOLD);
	}

	if (rl < COLS - 2) {
		attron(CP(CP_NUM) | A_BOLD);
		mvaddnstr(y, COLS - 1 - rl, right, rl);
		attroff(CP(CP_NUM) | A_BOLD);
	}
}

/*
 * Function: Draw the bottom function-key bar.
 * Parameters: None.
 * Return (void): No return value.
 */
static void draw_fkeys(void)
{
	int y = g_frect.y;
	int x = 1;
	int i;

	ui_fill_line(y, 0, COLS, CP(CP_BAR));
	for (i = 0; i < 10; i++) {
		int nl = (int)strlen(g_fkey_num[i]);
		int ll = (int)strlen(g_fkey_label[i]);

		if (x + nl + ll + 1 > COLS)
			break;
		attron(CP(CP_NUM) | A_BOLD);
		mvaddnstr(y, x, g_fkey_num[i], nl);
		attroff(CP(CP_NUM) | A_BOLD);
		attron(CP(CP_BAR) | A_BOLD);
		mvaddnstr(y, x + nl, g_fkey_label[i], ll);
		attroff(CP(CP_BAR) | A_BOLD);
		x += nl + ll + 1;
	}
}

/*
 * Function: Return the command belonging to a function-key label.
 * Parameters:
 *   x (int): Zero-based screen column in the bottom function-key bar.
 * Return (int): The matching Command, or CMD_NONE when the column is blank.
 */
static int fkey_command_at(int x)
{
	int start = 1, i;

	for (i = 0; i < 10; i++) {
		int width = (int)strlen(g_fkey_num[i]) +
			    (int)strlen(g_fkey_label[i]) + 1;

		if (start + width > COLS)
			break;
		if (x >= start && x < start + width)
			return g_fkey_cmd[i];
		start += width;
	}
	return CMD_NONE;
}

/*
 * Function: Redraw the complete main screen.
 * Parameters: None.
 * Return (void): No return value.
 */
static void draw_all(void)
{
	layout();
	erase();
	menubar_draw(-1);
	panel_draw(&g_panels[0], &g_prect[0], g_active == 0);
	panel_draw(&g_panels[1], &g_prect[1], g_active == 1);
	draw_status();
	draw_fkeys();
	refresh();
}

/* ------------------------------------------------------------------ *
 * Loading progress window                                              *
 * ------------------------------------------------------------------ */

/*
 * Function: Update the startup loading overlay.
 * Parameters:
 *   ud (void *): Opaque callback context; unused by the UI callback.
 *   msg (const char *): New progress message, or NULL to animate only.
 * Return (void): No return value.
 */
static void load_progress(void *ud, const char *msg)
{
	int h = 5, w = 64;
	long t = mono_ms();

	(void)ud;
	if (w > COLS - 2)
		w = COLS - 2;
	if (w < 20)
		w = COLS;
	if (h > LINES - 2)
		h = LINES;

	if (!g_load_win) {
		g_load_win = newwin(h, w, (LINES - h) / 2, (COLS - w) / 2);
		if (!g_load_win)
			return;
		wbkgd(g_load_win, CP(CP_DLG));
		ui_shadow(g_load_win);
		g_load_t0 = t;
		g_load_drawn = 0;
		g_load_msg[0] = '\0';
	}
	if (msg)
		fit_into(g_load_msg, sizeof(g_load_msg), msg);
	else if (t - g_load_drawn < 80)
		return; /* repaint request: keep ~12 spinner frames a second */
	g_load_drawn = t;

	werase(g_load_win);
	box(g_load_win, 0, 0);
	wattron(g_load_win, CP(CP_SEL) | A_BOLD);
	mvwaddstr(g_load_win, 0, 2, " mapt ");
	wattroff(g_load_win, CP(CP_SEL) | A_BOLD);
	/* "⠄ Listing available packages (apt list)..." */
	ui_draw_spinner(g_load_win, 2, 2, t - g_load_t0);
	mvwaddnstr(g_load_win, 2, 4, g_load_msg, (size_t)(w - 6));
	wrefresh(g_load_win);
}

/*
 * Function: Close the startup loading overlay.
 * Parameters: None.
 * Return (void): No return value.
 */
static void load_done(void)
{
	if (g_load_win) {
		ui_unshadow(g_load_win);
		delwin(g_load_win);
		g_load_win = NULL;
		touchwin(stdscr);
	}
}

/*
 * Function: Reload both package panels from the package database.
 * Parameters: None.
 * Return (void): No return value.
 */
static void reload_data(void)
{
	char *keep[2] = { NULL, NULL };
	int i, rc;

	/* The loading overlay changes the entire layout; discard input until
	 * the new package lists have been installed. */
	ui_mouse_set_enabled(0);
	for (i = 0; i < 2; i++) {
		Package *pk = panel_current(&g_panels[i]);

		if (pk)
			keep[i] = xstrdup(pk->name);
		panel_reset(&g_panels[i]);
	}

	g_origin_failed = 0;
	g_deps_failed = 0;
	load_progress(NULL, "Loading package database...");
	rc = pkgdb_load(&g_db, load_progress, NULL);
	load_done();

	for (i = 0; i < 2; i++) {
		panel_rebuild(&g_panels[i], &g_db, keep[i]);
		free(keep[i]);
		keep[i] = NULL;
	}

	ui_mouse_set_enabled(1);
	if (rc != 0)
		dlg_alert("Package database", pkgdb_error(), 1);
	else if (g_db.n == 0)
		dlg_alert("Package database", "The package database is "
					      "empty.", 1);
}

/* ------------------------------------------------------------------ *
 * Origins of the rows on screen                                       *
 * ------------------------------------------------------------------ */

/* "apt list" only knows the release a package comes from; ask
 * "apt-cache policy" about the rows the user can actually see (plus a
 * lookahead) to show the exact origin.  The answers are remembered, so
 * only new territory costs a fraction of a second.  Called when the
 * user pauses - never between two keys, so scrolling stays instant.
 * Returns 1 when anything was fetched, so the caller can repaint. */
/*
 * Function: Fetch exact origins for rows near both visible viewports.
 * Parameters: None.
 * Return (int): 1 when at least one origin was requested, otherwise 0.
 */
static int fetch_view_origins(void)
{
	char **names = NULL;
	size_t n = 0, cap = 0;
	int i;

	if (g_origin_failed)
		return 0;

	for (i = 0; i < 2; i++) {
		Panel *p = &g_panels[i];
		size_t end = p->top + p->rows + ORIGIN_LOOKAHEAD;
		size_t j;

		if (end > p->n)
			end = p->n;
		for (j = p->top; j < end; j++) {
			Package *pk = p->view ? p->view[j] : NULL;

			if (!pk || (pk->flags & PKGF_ORIGIN))
				continue;
			if (n == cap) {
				cap = cap ? cap * 2 : 128;
				names = xrealloc(names,
						 cap * sizeof(*names));
			}
			names[n++] = pk->name; /* borrowed */
		}
	}
	if (n && pkgdb_fetch_origins(&g_db, names, n) != 0)
		g_origin_failed = 1;
	free(names);
	return n > 0;
}

/* ------------------------------------------------------------------ *
 * Dependencies of the rows on screen                                  *
 * ------------------------------------------------------------------ */

#define DEPS_LOOKAHEAD 100

/* The "[+]" markers of the dependency tree come from "apt-cache
 * depends": resolve the viewport plus a lookahead in one batch, the
 * answers are remembered per package (PKGF_DEPS).  Returns 1 when
 * anything was fetched, so the caller can repaint the markers. */
/*
 * Function: Fetch dependency metadata for rows near both viewports.
 * Parameters: None.
 * Return (int): 1 when at least one dependency list was requested, otherwise 0.
 */
static int fetch_view_deps(void)
{
	char **names = NULL;
	size_t n = 0, cap = 0;
	int i, fetched = 0;

	if (g_deps_failed)
		return 0;

	for (i = 0; i < 2; i++) {
		Panel *p = &g_panels[i];
		size_t end = p->top + p->rows + DEPS_LOOKAHEAD;
		size_t j;

		if (end > p->n)
			end = p->n;
		for (j = p->top; j < end; j++) {
			Package *pk = p->view ? p->view[j] : NULL;
			size_t k;
			int dup = 0;

			if (!pk || (pk->flags & PKGF_DEPS))
				continue;
			/* the same package is a row of both panels and
			 * may repeat as a tree child */
			for (k = 0; k < n; k++)
				if (strcmp(names[k], pk->name) == 0) {
					dup = 1;
					break;
				}
			if (dup)
				continue;
			if (n == cap) {
				cap = cap ? cap * 2 : 128;
				names = xrealloc(names, cap * sizeof(*names));
			}
			names[n++] = pk->name; /* borrowed */
		}
	}
	if (n) {
		fetched = 1;
		if (pkgdb_fetch_deps(&g_db, names, n) != 0)
			g_deps_failed = 1;
	}
	free(names);
	return fetched;
}

/* ------------------------------------------------------------------ *
 * Actions                                                              *
 * ------------------------------------------------------------------ */

typedef int (*PkgPred)(const Package *);

/*
 * Function: Test whether a package can be installed or upgraded.
 * Parameters:
 *   pk (Package *): Package to test.
 * Return (int): Non-zero when the package is installable.
 */
static int pred_installable(const Package *pk)
{
	return !(pk->flags & PKGF_INSTALLED) ||
	       (pk->flags & PKGF_UPGRADEABLE);
}

/*
 * Function: Test whether a package is installed.
 * Parameters:
 *   pk (Package *): Package to test.
 * Return (int): Non-zero when the installed flag is set.
 */
static int pred_installed(const Package *pk)
{
	return (pk->flags & PKGF_INSTALLED) != 0;
}

/*
 * Function: Test whether a package has a pending upgrade.
 * Parameters:
 *   pk (Package *): Package to test.
 * Return (int): Non-zero when the upgradable flag is set.
 */
static int pred_upgradable(const Package *pk)
{
	return (pk->flags & PKGF_UPGRADEABLE) != 0;
}

static char **collect(Panel *p, PkgPred pred, size_t *out_n)
{
	char **v = NULL;
	size_t n = 0, cap = 0;
	size_t i;

	*out_n = 0;
	for (i = 0; i < p->n; i++) {
		Package *pk = p->view[i];

		if (!(pk->flags & PKGF_MARKED) || !pred(pk))
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 8;
			v = xrealloc(v, cap * sizeof(*v));
		}
		v[n++] = xstrdup(pk->name);
	}
	if (n == 0) {
		Package *pk = panel_current(p);

		if (pk && pred(pk)) {
			v = xrealloc(v, sizeof(*v));
			v[n++] = xstrdup(pk->name);
		}
	}
	*out_n = n;
	return v;
}

/*
 * Function: Free an array of package-name strings.
 * Parameters:
 *   v (char **): Array of n heap-allocated strings.
 *   n (size_t): Number of strings in v.
 * Return (void): No return value.
 */
static void names_free(char **v, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		free(v[i]);
	free(v);
}

static char *confirm_body(const char *verb, char **names, size_t n)
{
	Buf b;
	size_t i, limit = n < 12 ? n : 12;

	buf_init(&b);
	buf_printf(&b, "The following %zu package(s) will be %s:\n\n", n,
		   verb);
	for (i = 0; i < limit; i++)
		buf_printf(&b, "  %s\n", names[i]);
	if (n > limit)
		buf_printf(&b, "\n  ... and %zu more\n", n - limit);
	return buf_steal(&b);
}

/*
 * Function: Install or reinstall the active panel's selected packages.
 * Parameters:
 *   reinstall (int): Non-zero selects reinstall mode; zero selects install mode.
 * Return (void): No return value.
 */
static void do_install(int reinstall)
{
	Panel *p = &g_panels[g_active];
	size_t n = 0;
	char **names = collect(p, reinstall ? pred_installed :
					      pred_installable,
			       &n);
	char *body;
	const char *verb = reinstall ? "reinstalled" : "installed";
	int ok;

	if (n == 0) {
		dlg_alert(reinstall ? "Reinstall" : "Install",
			  reinstall ?
				  "No installed package is selected." :
				  "Nothing to install:\nthe selection is "
				  "already installed.\nUse F6 to upgrade or "
				  "the Command menu to reinstall.",
			  0);
		names_free(names, n);
		return;
	}

	body = confirm_body(verb, names, n);
	ok = dlg_confirm(reinstall ? "Reinstall" : "Install", body, 0);
	free(body);
	if (ok) {
		int st = reinstall ?
				 apt_reinstall("Reinstalling packages",
					       names, n) :
				 apt_install("Installing packages", names, n);

		if (st >= 0)
			reload_data();
	}
	names_free(names, n);
}

/*
 * Function: Upgrade the active panel's selected packages.
 * Parameters: None.
 * Return (void): No return value.
 */
static void do_upgrade(void)
{
	Panel *p = &g_panels[g_active];
	size_t n = 0;
	char **names = collect(p, pred_upgradable, &n);
	char *body;
	int ok;

	if (n == 0) {
		dlg_alert("Upgrade",
			  "No upgrade is available for the selection.",
			  0);
		names_free(names, n);
		return;
	}
	body = confirm_body("upgraded", names, n);
	ok = dlg_confirm("Upgrade", body, 1);
	free(body);
	if (ok) {
		int st = apt_install("Upgrading packages", names, n);

		if (st >= 0)
			reload_data();
	}
	names_free(names, n);
}

/*
 * Function: Remove or purge the active panel's selected packages.
 * Parameters:
 *   purge (int): Non-zero purges configuration files; zero removes packages.
 * Return (void): No return value.
 */
static void do_remove(int purge)
{
	Panel *p = &g_panels[g_active];
	size_t n = 0;
	char **names = collect(p, pred_installed, &n);
	char *body;
	int ok;

	if (n == 0) {
		dlg_alert(purge ? "Purge" : "Remove",
			  "No installed package is selected.", 0);
		names_free(names, n);
		return;
	}
	body = confirm_body(purge ? "purged (configuration files included)" :
				    "removed",
			    names, n);
	ok = dlg_confirm(purge ? "Purge" : "Remove", body, 0);
	free(body);
	if (ok) {
		int st = apt_remove(purge ? "Purging packages" :
					    "Removing packages",
				    names, n, purge);

		if (st >= 0)
			reload_data();
	}
	names_free(names, n);
}

/*
 * Function: Show information about the package in the active panel.
 * Parameters: None.
 * Return (void): No return value.
 */
static void do_info(void)
{
	Package *pk = panel_current(&g_panels[g_active]);
	char *title, *text;

	if (!pk) {
		dlg_alert("Info", "No package selected.", 0);
		return;
	}
	title = xasprintf("Info: %s", pk->name);
	/* Package details are fetched synchronously before the text window
	 * exists, so do not let a click queue up for the next screen. */
	ui_mouse_set_enabled(0);
	text = apt_pkg_info(pk);
	ui_mouse_set_enabled(1);
	dlg_text(title, text);
	free(title);
	free(text);
}

/*
 * Function: Show the file list for the package in the active panel.
 * Parameters: None.
 * Return (void): No return value.
 */
static void do_contents(void)
{
	Package *pk = panel_current(&g_panels[g_active]);
	char *title, *text;

	if (!pk) {
		dlg_alert("Files", "No package selected.", 0);
		return;
	}
	title = xasprintf("Files: %s", pk->name);
	/* The file list is also collected before its viewer is opened. */
	ui_mouse_set_enabled(0);
	text = apt_pkg_files(pk);
	ui_mouse_set_enabled(1);
	dlg_text(title, text);
	free(title);
	free(text);
}

/*
 * Function: Prompt for a package substring and search the active panel.
 * Parameters: None.
 * Return (void): No return value.
 */
static void do_find(void)
{
	char q[128];

	memset(q, 0, sizeof(q));
	if (dlg_input("Find package", "Find packages containing:", q,
		      sizeof(q), 0) != 0)
		return;
	if (!q[0])
		return;
	if (!panel_find_sub(&g_panels[g_active], q))
		dlg_alert("Find package", "No package matches.", 0);
}

/*
 * Function: Prompt for a glob and mark matching packages.
 * Parameters: None.
 * Return (void): No return value.
 */
static void do_mark_pattern(void)
{
	char pat[128];

	memset(pat, 0, sizeof(pat));
	if (dlg_input("Mark by pattern",
		      "Glob pattern (for example lib* or *-dev):", pat,
		      sizeof(pat), 0) != 0)
		return;
	if (!pat[0])
		return;
	panel_mark_pattern(&g_panels[g_active], pat, 1);
}

/*
 * Function: Show the application information dialog.
 * Parameters: None.
 * Return (void): No return value.
 */
static void show_about(void)
{
	char *t = xasprintf(
		"%s %s\n"
		"%s\n"
		"\n"
		"A two panel frontend for APT, written in C with ncurses.\n"
		"\n"
		"Left panel:  installed packages (menu: upgradable only)\n"
		"Right panel: not installed yet - the rest of the cache\n"
		"\n"
		"Enter, Right and Left expand and collapse the dependency\n"
		"tree; Tab or a mouse click switches the panels.\n"
		"\n"
		"Press F1 for the key reference.\n"
		"\n"
		"Author:  APPIT Adam Skowro\u00f1ski <info@appit.pl>\n",
		MAPT_NAME, MAPT_VERSION, MAPT_DESC);
	dlg_text("About", t);
	free(t);
}

/*
 * Function: Rebuild one panel from the current package database.
 * Parameters:
 *   i (int): Panel index: 0 for left or 1 for right.
 * Return (void): No return value.
 */
static void rebuild_panel(int i)
{
	panel_rebuild(&g_panels[i], &g_db, NULL);
}

/* ------------------------------------------------------------------ *
 * Command dispatch                                                     *
 * ------------------------------------------------------------------ */

/*
 * Function: Open the menu bar at a selected top-level entry.
 * Parameters:
 *   initial (int): Index of the menu to open; out-of-range values use File.
 * Return (int): The selected Command, or CMD_NONE when cancelled.
 */
static int run_menu(int initial)
{
	MenuState st;

	st.active = g_active;
	st.sort[0] = g_panels[0].sort;
	st.sort[1] = g_panels[1].sort;
	st.desc[0] = g_panels[0].desc;
	st.desc[1] = g_panels[1].desc;
	st.left_only_upgradeable = g_panels[PANEL_LEFT].only_upgradeable;
	return menubar_run(&st, initial);
}

/*
 * Function: Test a screen point against a layout rectangle.
 * Parameters:
 *   x (int): Zero-based screen column.
 *   y (int): Zero-based screen row.
 *   r (Rect *): Rectangle to test.
 * Return (int): Non-zero when (x, y) is inside r.
 */
static int point_in_rect(int x, int y, const Rect *r)
{
	return x >= r->x && x < r->x + r->w &&
	       y >= r->y && y < r->y + r->h;
}

/*
 * Function: Map the two wheel pseudo-buttons to a signed vertical direction.
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
 * Function: Translate a screen-relative mouse event into a command or a panel
 * navigation change.  The caller redraws after this function returns.
 * Parameters:
 *   ev (MEVENT *): Mouse event reported by getmouse().
 * Return (int): A Command for a bar action, or CMD_NONE for panel navigation.
 */
static int handle_mouse(const MEVENT *ev)
{
	int x = ev->x, y = ev->y;
	int left = ui_mouse_clicked(ev, 1);
	int right = ui_mouse_clicked(ev, 3);
	int double_click = (ev->bstate & BUTTON1_DOUBLE_CLICKED) != 0;
	int i;

	/* A non-left interaction breaks a pending manual double-click. */
	if (ev->bstate & (BUTTON2_PRESSED | BUTTON2_RELEASED |
			  BUTTON2_CLICKED | BUTTON2_DOUBLE_CLICKED |
			  BUTTON2_TRIPLE_CLICKED | BUTTON3_PRESSED |
			  BUTTON3_RELEASED | BUTTON3_CLICKED |
			  BUTTON3_DOUBLE_CLICKED | BUTTON3_TRIPLE_CLICKED |
			  BUTTON4_PRESSED | BUTTON5_PRESSED))
		g_click_panel = -1;
	if (y == 0 && left) {
		int menu = menubar_hit(x);

		g_click_panel = -1;
		return menu >= 0 ? run_menu(menu) : CMD_NONE;
	}
	if (left && point_in_rect(x, y, &g_frect)) {
		int cmd = fkey_command_at(x);

		g_click_panel = -1;
		return cmd;
	}

	for (i = 0; i < 2; i++) {
		const Rect *r = &g_prect[i];
		Panel *p = &g_panels[i];
		int row, wheel;

		if (!point_in_rect(x, y, r))
			continue;
		/* A click anywhere in a panel activates it, even when the
		 * point is on its frame or header. */
		g_active = i;
		row = y - (r->y + 2);
		wheel = mouse_wheel_dir(ev);
		if (wheel) {
			int step = p->rows < 3 ? (int)p->rows : 3;

			g_click_panel = -1;
			panel_scroll_rows(p, wheel * step);
		} else if (left) {
			/* The right border is the panel scrollbar track. */
			if (x == r->x + r->w - 1) {
				g_click_panel = -1;
				if (p->n > p->rows && row >= 0 &&
				    (size_t)row < p->rows)
					panel_scroll_to_row(p, row);
			} else if (panel_select_visible(p, row)) {
				/* A second click on the same row toggles its tree. */
				long now = mono_ms();

				if (double_click ||
				    (g_click_panel == i &&
				     g_click_index == p->cur &&
				     now >= g_click_time &&
				     now - g_click_time <= MOUSE_DOUBLE_MS)) {
					panel_tree_toggle(p, p->cur);
					g_click_panel = -1;
				} else {
					g_click_panel = i;
					g_click_index = p->cur;
					g_click_time = now;
				}
			} else {
				g_click_panel = -1;
			}
		} else if (right) {
			g_click_panel = -1;
			if (x != r->x + r->w - 1)
				panel_select_visible(p, row);
		}
		return CMD_NONE;
	}
	if (left)
		g_click_panel = -1;
	return CMD_NONE;
}

/*
 * Function: Execute one application command.
 * Parameters:
 *   cmd (int): Command value from cmd.h.
 * Return (int): Non-zero when the application should terminate.
 */
static int dispatch(int cmd)
{
	Panel *p = &g_panels[g_active];

	switch (cmd) {
	case CMD_NONE:
		return 0;
	case CMD_QUIT:
		return 1;
	case CMD_MENU: {
		int c = run_menu(0);

		if (c)
			return dispatch(c);
		return 0;
	}
	case CMD_HELP:
		dlg_help();
		return 0;
	case CMD_ABOUT:
		show_about();
		return 0;
	case CMD_INFO:
		do_info();
		return 0;
	case CMD_CONTENTS:
		do_contents();
		return 0;
	case CMD_FIND:
		do_find();
		return 0;
	case CMD_INSTALL:
		do_install(0);
		return 0;
	case CMD_REINSTALL:
		do_install(1);
		return 0;
	case CMD_UPGRADE:
		do_upgrade();
		return 0;
	case CMD_UPGRADE_ALL:
		if (apt_upgrade_all() >= 0)
			reload_data();
		return 0;
	case CMD_UPDATE_UPGRADE:
		if (apt_update_upgrade() >= 0)
			reload_data();
		return 0;
	case CMD_DIST_UPGRADE:
		if (apt_dist_upgrade() >= 0)
			reload_data();
		return 0;
	case CMD_REMOVE:
		do_remove(0);
		return 0;
	case CMD_PURGE:
		do_remove(1);
		return 0;
	case CMD_AUTOREMOVE:
		if (apt_autoremove() >= 0)
			reload_data();
		return 0;
	case CMD_UPDATE:
		if (apt_update() >= 0)
			reload_data();
		return 0;
	case CMD_RELOAD:
		reload_data();
		return 0;
	case CMD_MARK_TOGGLE:
		panel_toggle_mark(p);
		return 0;
	case CMD_MARK_PATTERN:
		do_mark_pattern();
		return 0;
	case CMD_MARK_CLEAR:
		panel_clear_marks(p);
		return 0;
	case CMD_SORT_NAME:
		p->sort = SORT_NAME;
		rebuild_panel(g_active);
		return 0;
	case CMD_SORT_VERSION:
		p->sort = SORT_VERSION;
		rebuild_panel(g_active);
		return 0;
	case CMD_SORT_SIZE:
		p->sort = SORT_SIZE;
		rebuild_panel(g_active);
		return 0;
	case CMD_SORT_REVERSE:
		p->desc = !p->desc;
		rebuild_panel(g_active);
		return 0;
	case CMD_PANEL_MODE:
		g_panels[PANEL_LEFT].only_upgradeable =
			!g_panels[PANEL_LEFT].only_upgradeable;
		rebuild_panel(PANEL_LEFT);
		return 0;
	default:
		return 0;
	}
}

/* ------------------------------------------------------------------ *
 * Main                                                                 *
 * ------------------------------------------------------------------ */

/*
 * Function: Print command-line usage information.
 * Parameters:
 *   out (FILE *): Output stream receiving the usage text.
 * Return (void): No return value.
 */
static void usage(FILE *out)
{
	fprintf(out,
		"%s %s - %s\n"
		"usage: %s [-h] [-V]\n"
		"  -h, --help     show this help\n"
		"  -V, --version  show the version\n",
		MAPT_NAME, MAPT_VERSION, MAPT_DESC, MAPT_NAME);
}

/*
 * Function: Parse command-line options and run the interactive interface.
 * Parameters:
 *   argc (int): Number of command-line arguments.
 *   argv (char **): Array of command-line argument strings.
 * Return (int): Process exit status: 0 on normal exit, 1 after usage errors.
 */
int main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 ||
		    strcmp(argv[i], "--help") == 0) {
			usage(stdout);
			return 0;
		}
		if (strcmp(argv[i], "-V") == 0 ||
		    strcmp(argv[i], "--version") == 0) {
			printf("%s %s\n", MAPT_NAME, MAPT_VERSION);
			return 0;
		}
		fprintf(stderr, "%s: unknown option: %s\n", MAPT_NAME,
			argv[i]);
		usage(stderr);
		return 2;
	}

	signal(SIGPIPE, SIG_IGN);
	setlocale(LC_ALL, "");

	pkgdb_init(&g_db);
	panel_init(&g_panels[0], PANEL_LEFT);
	panel_init(&g_panels[1], PANEL_RIGHT);
	g_active = 0;

	initscr();
	cbreak();
	noecho();
	keypad(stdscr, TRUE);
	curs_set(0);
	timeout(1000);
	ui_init_colors();

	reload_data();
	clock_str(g_clock, sizeof(g_clock));
	/* Paint first: the panels only learn their height from the first
	 * draw, so fetching before it would cover just the lookahead and
	 * the leftover rows would spawn apt-cache at the first pause. */
	ui_mouse_set_enabled(0);
	if (!too_small())
		draw_all();
	fetch_view_origins();
	fetch_view_deps();
	ui_mouse_set_enabled(1);

	for (;;) {
		int key, cmd = CMD_NONE;

		if (too_small()) {
			int key;

			draw_small();
			key = getch();
			/* Consume mouse reports even while the terminal is too
			 * small, so they cannot leak into the next layout. */
			if (key == KEY_MOUSE) {
				MEVENT ev;

				if (getmouse(&ev) == OK) {
					ui_mouse_clicked(&ev, 1);
					ui_mouse_clicked(&ev, 3);
				}
			}
			continue;
		}

		draw_all();

		key = getch();
		if (key == ERR) {
			char now[16];
			int fetched = 0;

			clock_str(now, sizeof(now));
			if (strcmp(now, g_clock) != 0) {
				memcpy(g_clock, now, sizeof(now));
				menubar_draw(-1);
				refresh();
			}
			/* The user paused: fill the origins and the [+]
			 * markers of whatever scrolled into the viewport.
			 * While keys keep arriving this branch never runs,
			 * so scrolling never waits for apt-cache. */
			if (fetch_view_origins()) {
				draw_all();
				fetched = 1;
			}
			if (fetch_view_deps()) {
				draw_all();
				fetched = 1;
			}
			if (fetched) {
				/* Do not replay clicks received while the
				 * background policy/depends calls ran. */
				ui_mouse_set_enabled(0);
				ui_mouse_set_enabled(1);
			}
			continue;
		}

		/* Mouse events use the same command dispatch path as keys. */
		if (key == KEY_MOUSE) {
			MEVENT ev;

			if (getmouse(&ev) == OK)
				cmd = handle_mouse(&ev);
		} else {
			g_click_panel = -1;
			switch (key) {
			case KEY_F(1):
				cmd = CMD_HELP;
				break;
			case KEY_F(2):
			case KEY_F(9):
				cmd = CMD_MENU;
				break;
			case KEY_F(3):
				cmd = CMD_INFO;
				break;
			case '\n':
			case KEY_ENTER: /* toggles the dependency tree */
				panel_tree_toggle(&g_panels[g_active],
						  g_panels[g_active].cur);
				break;
			case KEY_F(4):
				cmd = CMD_CONTENTS;
				break;
			case KEY_F(5):
				cmd = CMD_INSTALL;
				break;
			case KEY_F(6):
				cmd = CMD_UPGRADE;
				break;
			case KEY_F(7):
				cmd = CMD_FIND;
				break;
			case KEY_F(8):
			case KEY_DC:
				cmd = CMD_REMOVE;
				break;
			case KEY_F(10):
				cmd = CMD_QUIT;
				break;
			case '\t':
			case KEY_BTAB: /* the only keys that switch panels */
				g_active = g_active ? 0 : 1;
				break;
			case 18: /* Ctrl+R */
				cmd = CMD_RELOAD;
				break;
			default:
				cmd = panel_handle_key(&g_panels[g_active], key);
				break;
			}
		}

		if (cmd && dispatch(cmd))
			break;
	}

	endwin();
	panel_free(&g_panels[0]);
	panel_free(&g_panels[1]);
	pkgdb_free(&g_db);
	return 0;
}
