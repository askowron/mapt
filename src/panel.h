/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 APPIT Adam Skowroński
 */
#ifndef MAPT_PANEL_H
#define MAPT_PANEL_H

#include <stddef.h>
#include "cmd.h"
#include "pkg.h"
#include "ui.h"

typedef enum {
	PANEL_LEFT = 0,
	PANEL_RIGHT = 1
} PanelSide;

typedef enum {
	SORT_NAME = 0,
	SORT_VERSION,
	SORT_SIZE
} SortKey;

typedef struct {
	PanelSide side;
	Package **view;      /* rows of the panel, pointers into PkgDB */
	unsigned char *depth; /* dependency tree depth per row, parallel to view */
	PkgDB *db;  /* set by panel_rebuild(), used by the tree code */
	size_t n, cap;
	size_t n_tree; /* rows with depth > 0 (they break the sort order) */
	size_t cur, top;
	size_t rows; /* visible rows, updated by panel_draw() */
	char search[64];
	size_t search_len;
	SortKey sort;
	int desc;
	int only_upgradeable; /* left panel filter (Options menu) */
	size_t marked;
} Panel;

void panel_init(Panel *p, PanelSide side);
void panel_free(Panel *p);
void panel_reset(Panel *p); /* drop rows before the database is reloaded */

/* Rebuild the row list from the database.  keep_name (may be NULL)
 * restores the cursor position by package name.  Rows of packages left
 * expanded in the tree are restored. */
void panel_rebuild(Panel *p, PkgDB *db, const char *keep_name);

void panel_draw(Panel *p, const Rect *r, int active);

Package *panel_current(const Panel *p);

/* Consumes the key and returns CMD_NONE, or returns the command the
 * caller has to execute (0 when the key was not for us). */
int panel_handle_key(Panel *p, int key);

void panel_toggle_mark(Panel *p);
void panel_mark_pattern(Panel *p, const char *pattern, int mark);
void panel_clear_marks(Panel *p);
int panel_select_name(Panel *p, const char *name);
int panel_find_sub(Panel *p, const char *needle);
void panel_page(Panel *p, int dir);

/* Mouse navigation.  p is the panel to update.  select_visible() takes a
 * zero-based row in the current viewport; scroll_rows() takes a signed row
 * count; scroll_to_row() takes a row on the scrollbar track. */
int panel_select_visible(Panel *p, int row);
void panel_scroll_rows(Panel *p, int rows);
void panel_scroll_to_row(Panel *p, int row);

/* Dependency tree: a row whose package has Depends/PreDepends shows a
 * "[+]" ("[-]" when expanded) marker; the rows below it are its direct
 * dependencies.  Unknown dependency lists are fetched synchronously. */
void panel_tree_toggle(Panel *p, size_t idx);
void panel_tree_expand(Panel *p, size_t idx);
void panel_tree_collapse(Panel *p, size_t idx);

#endif
