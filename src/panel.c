#include "panel.h"
#include "util.h"
#include "vercmp.h"

#include <ctype.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Fixed zone for the "[+]"/"[-]" dependency tree marker, between the
 * mark column and the package name. */
#define TREE_W 4

/* Hard cap of the tree nesting (also the backstop of the cycle guard). */
#define TREE_MAX_DEPTH 32

/* qsort() comparators need the sort mode somewhere; mapt is single
 * threaded so a file static is enough. */
static SortKey g_sort;
static int g_desc;

static void clamp_view(Panel *p);

static const char *pkg_version(const Package *p)
{
	if (p->installed)
		return p->installed;
	if (p->candidate)
		return p->candidate;
	return "";
}

/*
 * Function: Compare two package pointers for qsort().
 * Parameters:
 *   a (const void *): Pointer to a Package * comparator operand.
 *   b (const void *): Pointer to the other Package * comparator operand.
 * Return (int): Negative, zero, or positive ordering result.
 */
static int cmp_pkg(const void *a, const void *b)
{
	const Package *pa = *(const Package *const *)a;
	const Package *pb = *(const Package *const *)b;
	int r = 0;

	switch (g_sort) {
	case SORT_VERSION:
		r = dpkg_vercmp(pkg_version(pa), pkg_version(pb));
		break;
	case SORT_SIZE:
		if (pa->size_kib != pb->size_kib)
			r = pa->size_kib < pb->size_kib ? -1 : 1;
		break;
	case SORT_NAME:
	default:
		break;
	}
	if (r == 0)
		return strcasecmp(pa->name, pb->name);
	return g_desc ? -r : r;
}

/*
 * Function: Initialize an empty package panel.
 * Parameters:
 *   p (Panel *): Panel storage to initialize.
 *   side (PanelSide): PANEL_LEFT or PANEL_RIGHT.
 * Return (void): No return value.
 */
void panel_init(Panel *p, PanelSide side)
{
	memset(p, 0, sizeof(*p));
	p->side = side;
	p->sort = SORT_NAME;
	p->rows = 10;
}

/*
 * Function: Release all storage owned by a panel.
 * Parameters:
 *   p (Panel *): Panel to release and leave empty.
 * Return (void): No return value.
 */
void panel_free(Panel *p)
{
	free(p->view);
	p->view = NULL;
	free(p->depth);
	p->depth = NULL;
	p->n = p->cap = 0;
	p->n_tree = 0;
	p->db = NULL;
}

/* Forget the rows without touching the stored pointers: used right
 * before the PkgDB is reloaded, so that no dangling pointer is ever
 * dereferenced. */
/*
 * Function: Drop panel rows before their backing database is replaced.
 * Parameters:
 *   p (Panel *): Panel to clear without releasing its storage.
 * Return (void): No return value.
 */
void panel_reset(Panel *p)
{
	p->n = 0;
	p->n_tree = 0;
	p->cur = p->top = 0;
	p->marked = 0;
	p->search_len = 0;
	p->search[0] = '\0';
}

/*
 * Function: Return the package under the panel cursor.
 * Parameters:
 *   p (Panel *): Panel to query.
 * Return (Package *): Borrowed package pointer, or NULL when no row exists.
 */
Package *panel_current(const Panel *p)
{
	if (!p->view || p->n == 0 || p->cur >= p->n)
		return NULL;
	return p->view[p->cur];
}

/* The panels are a strict partition of the database: installed
 * packages live on the left (optionally only the upgradable ones),
 * everything else on the right - a package never shows up twice as a
 * base row. */
/*
 * Function: Test whether a package belongs in a panel.
 * Parameters:
 *   p (Panel *): Panel whose side and filter are tested.
 *   pk (Package *): Package to test.
 * Return (int): Non-zero when the package is included in the panel.
 */
static int matches(const Panel *p, const Package *pk)
{
	int installed = (pk->flags & PKGF_INSTALLED) != 0;

	if (p->side == PANEL_LEFT) {
		if (!installed)
			return 0;
		if (p->only_upgradeable)
			return (pk->flags & PKGF_UPGRADEABLE) != 0;
		return 1;
	}
	return !installed;
}

/* ------------------------------------------------------------------ *
 * Dependency tree                                                      *
 * ------------------------------------------------------------------ */

/* Make room for at least need rows; the row list and the depth list
 * always grow together. */
/*
 * Function: Reserve parallel view and depth storage for panel rows.
 * Parameters:
 *   p (Panel *): Panel whose row storage should grow.
 *   need (size_t): Required number of row slots.
 * Return (void): No return value.
 */
static void row_reserve(Panel *p, size_t need)
{
	if (need <= p->cap)
		return;
	p->cap = p->cap ? p->cap * 2 : 256;
	while (p->cap < need)
		p->cap *= 2;
	p->view = xrealloc(p->view, p->cap * sizeof(*p->view));
	p->depth = xrealloc(p->depth, p->cap * sizeof(*p->depth));
}

/*
 * Function: Fetch the dependency list of pk when it is still unknown.
 * Parameters:
 *   p (Panel *): Panel whose database supplies dependency information.
 *   pk (Package *): Package whose dependency list is requested.
 * Return (int): 1 when the list is available, otherwise 0.
 */
static int ensure_deps(Panel *p, Package *pk)
{
	char *names[1];
	int ok;

	if (!p->db)
		return 0;
	if (pk->flags & PKGF_DEPS)
		return 1;
	names[0] = pk->name;
	/* Dependency discovery is synchronous; do not replay a click made
	 * while apt-cache is running against the expanded tree. */
	ui_mouse_set_enabled(0);
	ok = pkgdb_fetch_deps(p->db, names, 1) == 0;
	ui_mouse_set_enabled(1);
	return ok;
}

/* Index behind the subtree of the row at idx. */
/*
 * Function: Find the first row after a dependency subtree.
 * Parameters:
 *   p (Panel *): Panel containing the tree.
 *   idx (size_t): Index of the subtree root.
 * Return (size_t): Index immediately after the subtree.
 */
static size_t subtree_end(const Panel *p, size_t idx)
{
	int d = p->depth[idx];
	size_t i = idx + 1;

	while (i < p->n && p->depth[i] > d)
		i++;
	return i;
}

/* Keep cur/top pointing at the same rows when rows were inserted
 * (delta > 0) or removed (delta < 0) right after pivot. */
/*
 * Function: Keep cursor and viewport anchored across tree row changes.
 * Parameters:
 *   p (Panel *): Panel whose indexes should be adjusted.
 *   pivot (size_t): Index at which rows were inserted or removed.
 *   delta (long): Positive insertion count or negative removal count.
 * Return (void): No return value.
 */
static void reanchor(Panel *p, size_t pivot, long delta)
{
	if (delta > 0) {
		if (p->cur > pivot)
			p->cur += (size_t)delta;
		if (p->top > pivot)
			p->top += (size_t)delta;
	} else if (delta < 0) {
		size_t rem = (size_t)(-delta);

		if (p->cur > pivot)
			p->cur = p->cur - rem > pivot ? p->cur - rem : pivot;
		if (p->top > pivot)
			p->top = p->top - rem > pivot ? p->top - rem : pivot;
	}
	clamp_view(p);
}

/* Insert the children of the row at idx directly after it (depth + 1),
 * recursively restoring children that are expanded as well.  path[]
 * holds the packages from the top ancestor down to the row itself -
 * a child already on that path would close a cycle; dpath is the
 * count.  Returns the number of rows added. */
static size_t tree_insert(Panel *p, size_t idx, Package **path,
			  size_t dpath)
{
	const Package *pk = p->view[idx];
	int depth = p->depth[idx];
	size_t added = 0, off = 0;
	size_t i, k;

	if (!(pk->flags & PKGF_DEPS) || pk->n_deps == 0)
		return 0;
	if (dpath > TREE_MAX_DEPTH)
		return 0;

	for (i = 0; i < pk->n_deps; i++) {
		Package *child = p->db ? pkgdb_find(p->db, pk->deps[i]) :
					 NULL;
		size_t j;
		int on_path = 0;

		if (!child)
			continue;
		for (j = 0; j < dpath; j++)
			if (path[j] == child) {
				on_path = 1;
				break;
			}
		if (on_path)
			continue;

		/* make room right after the parent (and its earlier
		 * children) and drop the new row into it */
		row_reserve(p, p->n + 1);
		memmove(&p->view[idx + added + 2],
			&p->view[idx + added + 1],
			(p->n - (idx + added + 1)) * sizeof(*p->view));
		memmove(&p->depth[idx + added + 2],
			&p->depth[idx + added + 1],
			(p->n - (idx + added + 1)) * sizeof(*p->depth));
		p->view[idx + added + 1] = child;
		p->depth[idx + added + 1] = (unsigned char)(depth + 1);
		p->n++;
		p->n_tree++;
		added++;
	}

	/* Children remembered as expanded keep their state. */
	for (k = 0; k < added; k++) {
		size_t cidx = idx + 1 + k + off;
		Package *child = p->view[cidx];

		if ((child->flags & PKGF_EXPANDED) && child->n_deps) {
			Package *sub[TREE_MAX_DEPTH + 1];
			size_t j;

			for (j = 0; j < dpath; j++)
				sub[j] = path[j];
			sub[dpath] = child;
			off += tree_insert(p, cidx, sub, dpath + 1);
		}
	}
	return added + off;
}

/* Remove the rows of the subtree of the row at idx (the row itself
 * stays) and mark the node as collapsed. */
/*
 * Function: Remove dependency rows below one tree node.
 * Parameters:
 *   p (Panel *): Panel containing the expanded tree.
 *   idx (size_t): Index of the node to collapse.
 * Return (void): No return value.
 */
static void tree_collapse_at(Panel *p, size_t idx)
{
	size_t end = subtree_end(p, idx);
	size_t removed = end - (idx + 1);
	Package *pk = p->view[idx];

	if (removed) {
		memmove(&p->view[idx + 1], &p->view[end],
			(p->n - end) * sizeof(*p->view));
		memmove(&p->depth[idx + 1], &p->depth[end],
			(p->n - end) * sizeof(*p->depth));
		p->n -= removed;
		p->n_tree -= removed; /* every removed row had depth > 0 */
		reanchor(p, idx, -(long)removed);
	}
	pk->flags &= (unsigned char)~PKGF_EXPANDED;
}

/*
 * Function: Toggle dependency-tree expansion for one row.
 * Parameters:
 *   p (Panel *): Panel containing the row.
 *   idx (size_t): Zero-based index into the panel view.
 * Return (void): No return value.
 */
void panel_tree_toggle(Panel *p, size_t idx)
{
	Package *pk, *path[TREE_MAX_DEPTH + 1];
	size_t added;

	if (!p->db || idx >= p->n)
		return;
	pk = p->view[idx];
	if (pk->flags & PKGF_EXPANDED) {
		tree_collapse_at(p, idx);
		return;
	}
	if (!ensure_deps(p, pk))
		return;
	if (pk->n_deps == 0)
		return;
	path[0] = pk;
	added = tree_insert(p, idx, path, 1);
	if (added) {
		pk->flags |= PKGF_EXPANDED;
		reanchor(p, idx, (long)added);
	}
}

/*
 * Function: Expand dependency children for one panel row.
 * Parameters:
 *   p (Panel *): Panel containing the row.
 *   idx (size_t): Zero-based index into the panel view.
 * Return (void): No return value.
 */
void panel_tree_expand(Panel *p, size_t idx)
{
	Package *pk, *path[TREE_MAX_DEPTH + 1];
	size_t added;

	if (!p->db || idx >= p->n)
		return;
	pk = p->view[idx];
	if (pk->flags & PKGF_EXPANDED)
		return;
	if (!ensure_deps(p, pk) || pk->n_deps == 0)
		return;
	path[0] = pk;
	added = tree_insert(p, idx, path, 1);
	if (added) {
		pk->flags |= PKGF_EXPANDED;
		reanchor(p, idx, (long)added);
	}
}

/*
 * Function: Collapse dependency children for one panel row.
 * Parameters:
 *   p (Panel *): Panel containing the row.
 *   idx (size_t): Zero-based index into the panel view.
 * Return (void): No return value.
 */
void panel_tree_collapse(Panel *p, size_t idx)
{
	if (idx >= p->n)
		return;
	if (p->view[idx]->flags & PKGF_EXPANDED)
		tree_collapse_at(p, idx);
}

/*
 * Function: Rebuild the panel row list from a package database.
 * Parameters:
 *   p (Panel *): Panel to rebuild.
 *   db (PkgDB *): Database supplying package rows.
 *   keep_name (const char *): Optional package name whose cursor position
 *     should be restored.
 * Return (void): No return value.
 */
void panel_rebuild(Panel *p, PkgDB *db, const char *keep_name)
{
	char *saved = NULL;
	size_t i;

	if (keep_name)
		saved = xstrdup(keep_name);
	else {
		Package *old = panel_current(p);

		if (old)
			saved = xstrdup(old->name);
	}

	p->n = 0;
	p->n_tree = 0;
	p->marked = 0;
	p->search_len = 0;
	p->search[0] = '\0';
	p->db = db;

	if (db) {
		for (i = 0; i < db->n; i++) {
			Package *pk = &db->v[i];

			if (!matches(p, pk))
				continue;
			row_reserve(p, p->n + 1);
			p->depth[p->n] = 0;
			p->view[p->n++] = pk;
			if (pk->flags & PKGF_MARKED)
				p->marked++;
		}
	}

	g_sort = p->sort;
	g_desc = p->desc;
	if (p->n > 1)
		qsort(p->view, p->n, sizeof(*p->view), cmp_pkg);

	/* Rebuild the trees that were expanded before (the flags live in
	 * the packages, so sorting and reloading keep the state). */
	for (i = 0; i < p->n; i++) {
		Package *pk = p->view[i];
		Package *path[TREE_MAX_DEPTH + 1];

		if (p->depth[i] != 0 || !(pk->flags & PKGF_EXPANDED))
			continue;
		path[0] = pk;
		tree_insert(p, i, path, 1);
	}

	p->cur = 0;
	p->top = 0;
	if (saved) {
		panel_select_name(p, saved);
		free(saved);
	}
	if (p->n && p->cur >= p->n)
		p->cur = p->n - 1;
}

/*
 * Function: Move the panel cursor to a named package.
 * Parameters:
 *   p (Panel *): Panel to search.
 *   name (const char *): Exact package name to select.
 * Return (int): 1 when found, otherwise 0.
 */
int panel_select_name(Panel *p, const char *name)
{
	size_t i, found = (size_t)-1;

	if (!name)
		return 0;
	for (i = 0; i < p->n; i++) {
		if (strcmp(p->view[i]->name, name) != 0)
			continue;
		/* a package can also be a child row: prefer the base row */
		if (p->depth[i] == 0) {
			p->cur = i;
			return 1;
		}
		if (found == (size_t)-1)
			found = i;
	}
	if (found != (size_t)-1) {
		p->cur = found;
		return 1;
	}
	return 0;
}

/*
 * Function: Clamp cursor and viewport indexes to valid panel bounds.
 * Parameters:
 *   p (Panel *): Panel to normalize.
 * Return (void): No return value.
 */
static void clamp_view(Panel *p)
{
	if (p->n == 0) {
		p->cur = p->top = 0;
		return;
	}
	if (p->rows == 0)
		p->rows = 1;
	if (p->cur >= p->n)
		p->cur = p->n - 1;
	if (p->cur < p->top)
		p->top = p->cur;
	if (p->cur >= p->top + p->rows)
		p->top = p->cur - p->rows + 1;
	if (p->top + p->rows > p->n)
		p->top = p->n > p->rows ? p->n - p->rows : 0;
}

/* ------------------------------------------------------------------ *
 * Column layout                                                        *
 * ------------------------------------------------------------------ */

/*
 * Three columns: Package | Version | Size/Origin.  The package name has
 * the priority: first the extra column shrinks down to min_ext, then the
 * version, and when even that is not enough the extra column is dropped
 * completely - a ten character origin is not worth showing (the full
 * text is always in the status line and in the info dialog).
 */
static void col_layout(int inner_w, int want_ext, int min_ext, int *name_w,
		       int *ver_w, int *ext_w)
{
	int avail = inner_w - 1 - TREE_W; /* mark + tree marker columns */
	int vw = 18;
	int ew = want_ext;
	int nw;

	if (avail < 8) {
		*name_w = avail > 0 ? avail : 1;
		*ver_w = 0;
		*ext_w = 0;
		return;
	}

	nw = avail - vw - ew - 2;
	if (nw < 18) {
		int need = 18 - nw;
		int s = ew - min_ext;

		if (s > need)
			s = need;
		if (s > 0) {
			ew -= s;
			need -= s;
		}
		if (need > 0) {
			s = vw - 12;
			if (s > need)
				s = need;
			vw -= s;
			need -= s;
		}
		nw = avail - vw - ew - 2;
	}
	if (nw < 18 && ew > 0) { /* give the extra column up */
		ew = 0;
		vw = 18;
		nw = avail - vw - ew - 2;
	}
	if (nw < 18 && vw > 10) { /* and shrink the version instead */
		vw = 10;
		nw = avail - vw - ew - 2;
	}
	if (nw < 4) { /* extremely narrow: only the name survives */
		ew = 0;
		nw = avail > 6 ? avail - 6 : 4;
		vw = avail - nw - 2;
		if (vw < 0) {
			vw = 0;
			nw = avail;
		}
	}

	*name_w = nw;
	*ver_w = vw;
	*ext_w = ew;
}

/* ------------------------------------------------------------------ *
 * Drawing                                                              *
 * ------------------------------------------------------------------ */

/* Write s right aligned inside the w cells starting at (y, x).  The row
 * background was already painted by ui_fill_line(), so the padding on
 * the left needs no extra work. */
/*
 * Function: Draw text right-aligned inside a cell range.
 * Parameters:
 *   y (int): Target screen row.
 *   x (int): Starting screen column of the range.
 *   w (int): Width of the range in cells.
 *   s (const char *): NUL-terminated text to draw.
 * Return (void): No return value.
 */
static void add_right(int y, int x, int w, const char *s)
{
	int len = (int)strlen(s);

	if (len >= w)
		mvaddnstr(y, x, s, w);
	else
		mvaddnstr(y, x + (w - len), s, len);
}

static void draw_row_text(const Package *pk, const Panel *p, size_t idx,
			  int y, int x, int name_w, int ver_w, int ext_w)
{
	char nbuf[512], vbuf[512], ebuf[512], sbuf[40], zone[8];
	const char *ver;
	const char *ext;
	char mark = (pk->flags & PKGF_MARKED) ? '*' : ' ';
	int ind;

	if (p->side == PANEL_LEFT) {
		ver = pk->installed ? pk->installed : "-";
		if ((pk->flags & PKGF_INSTALLED) && pk->size_kib) {
			human_kib(pk->size_kib, sbuf, sizeof(sbuf));
			ext = sbuf;
		} else {
			ext = "-";
		}
	} else {
		ver = pk->candidate ? pk->candidate : "-";
		if (pk->repo)
			ext = pk->repo;
		else
			ext = (pk->flags & PKGF_INSTALLED) ? "(local)" : "-";
	}

	/* One tab (four cells) of indentation per tree level: the marker
	 * and the name of a child row both shift right, so the levels
	 * stack up like a tree ("    [+] child"). */
	ind = (int)p->depth[idx] * 4;
	if (ind > name_w - 1)
		ind = name_w - 1;

	mvaddch(y, x, (chtype)(unsigned char)mark);

	/* dependency tree zone: "[+] ", "[-] " or four spaces */
	if ((pk->flags & PKGF_DEPS) && pk->n_deps > 0)
		snprintf(zone, sizeof(zone), "%s ",
			 (pk->flags & PKGF_EXPANDED) ? "[-]" : "[+]");
	else
		memcpy(zone, "    ", 5);
	mvaddnstr(y, x + 1 + ind, zone, TREE_W);

	fit_into(nbuf, (size_t)(name_w - ind) + 1, pk->name);
	mvaddnstr(y, x + 1 + ind + TREE_W, nbuf, name_w - ind);

	if (ver_w > 0) {
		fit_into(vbuf, (size_t)ver_w + 1, ver);
		mvaddnstr(y, x + 1 + TREE_W + name_w + 1, vbuf, ver_w);
	}
	if (ext_w > 0) {
		fit_into(ebuf, (size_t)ext_w + 1, ext);
		/* the Size column is numeric: numbers read better
		 * flush against the right edge of their column */
		if (p->side == PANEL_LEFT)
			add_right(y, x + 1 + TREE_W + name_w + 1 + ver_w + 1,
				  ext_w, ebuf);
		else
			mvaddnstr(y,
				  x + 1 + TREE_W + name_w + 1 + ver_w + 1,
				  ebuf, ext_w);
	}
}

/* The right border doubles as the scrollbar: a proportional thumb on
 * the ACS_VLINE track, drawn whenever the view does not fit, so no
 * column is taken away from the rows.  The track keeps whatever
 * ui_draw_frame() painted. */
/*
 * Function: Draw the panel scrollbar thumb on its right border.
 * Parameters:
 *   p (Panel *): Panel whose viewport position is represented.
 *   r (Rect *): Panel screen rectangle.
 * Return (void): No return value.
 */
static void draw_scrollbar(const Panel *p, const Rect *r)
{
	int x = r->x + r->w - 1;
	int y0 = r->y + 2; /* top border + header row */
	int h = (int)p->rows;
	size_t tl, off, i;

	if (h < 1 || p->n <= p->rows)
		return;

	/* at least one cell, otherwise proportional to the viewport */
	tl = (size_t)h * (size_t)h / p->n;
	if (tl < 1)
		tl = 1;
	/* clamp_view() keeps top <= n - rows, so the thumb lands
	 * exactly at the bottom for the last page */
	off = (p->top * ((size_t)h - tl)) / (p->n - p->rows);

	attron(CP(CP_NUM) | A_BOLD);
	for (i = 0; i < tl; i++)
		mvaddstr(y0 + (int)(off + i), x, "\u2588");
	attroff(CP(CP_NUM) | A_BOLD);
}

/*
 * Function: Draw one package panel and update its visible-row count.
 * Parameters:
 *   p (Panel *): Panel to render.
 *   r (Rect *): Screen rectangle occupied by the panel.
 *   active (int): Non-zero draws the focused cursor row.
 * Return (void): No return value.
 */
void panel_draw(Panel *p, const Rect *r, int active)
{
	int inner_x = r->x + 1;
	int inner_y = r->y + 1;
	int inner_w = r->w - 2;
	int list_h = r->h - 3; /* top border + header + bottom border */
	int hdr_attr = active ? CP(CP_SEL) : (CP(CP_PANEL) | A_BOLD);
	int name_w, ver_w, ext_w;
	int name_x, ver_x, ext_x;
	char title[64];
	char right[32];
	int i;

	if (r->w < 6 || r->h < 5)
		return;
	if (list_h < 1)
		list_h = 1;

	p->rows = (size_t)list_h;
	clamp_view(p);

	if (p->only_upgradeable)
		snprintf(title, sizeof(title), " Upgradable ");
	else if (p->side == PANEL_LEFT)
		snprintf(title, sizeof(title), " Installed ");
	else
		snprintf(title, sizeof(title), " Available ");
	snprintf(right, sizeof(right), "(%zu)", p->n);

	ui_draw_frame(r, title, right, hdr_attr);

	col_layout(inner_w, p->side == PANEL_LEFT ? 8 : 26,
		   p->side == PANEL_LEFT ? 7 : 16, &name_w, &ver_w, &ext_w);
	name_x = inner_x + 1 + TREE_W; /* mark + tree marker columns */
	ver_x = name_x + name_w + 1;
	ext_x = ver_x + ver_w + 1;

	/* column header */
	ui_fill_line(inner_y, inner_x, inner_w, hdr_attr);
	attron(hdr_attr);
	mvaddnstr(inner_y, name_x, "Package", name_w);
	if (ver_w > 0)
		mvaddnstr(inner_y, ver_x, "Version", ver_w);
	if (ext_w > 0) {
		if (p->side == PANEL_LEFT)
			add_right(inner_y, ext_x, ext_w, "Size");
		else
			mvaddnstr(inner_y, ext_x, "Origin", ext_w);
	}
	attroff(hdr_attr);

	/* rows */
	for (i = 0; i < list_h; i++) {
		size_t idx = p->top + (size_t)i;
		int y = inner_y + 1 + i;
		Package *pk;
		int attr;

		if (idx >= p->n)
			break;
		pk = p->view[idx];

		if (active && idx == p->cur)
			attr = CP(CP_SEL); /* the inactive panel shows no
					     * cursor row: it appears as any
					     * other row and the decorations
					     * (mark, upgrade) still apply */
		else if (pk->flags & PKGF_MARKED)
			attr = CP(CP_MARK);
		else if (pk->flags & PKGF_UPGRADEABLE)
			attr = CP(CP_UPGR);
		else if (p->depth[idx] > 0 && (pk->flags & PKGF_INSTALLED))
			attr = CP(CP_DEP); /* installed child row */
		else
			attr = CP(CP_PANEL);

		ui_fill_line(y, inner_x, inner_w, attr);
		attron(attr);
		draw_row_text(pk, p, idx, y, inner_x, name_w, ver_w, ext_w);
		attroff(attr);
	}

	draw_scrollbar(p, r);

	/* labels on the bottom border */
	{
		char bottom[64], marks[32];
		const char *sortname = p->sort == SORT_VERSION ?
					       "version" :
				       p->sort == SORT_SIZE ? "size" :
							      "name";

		snprintf(bottom, sizeof(bottom), " srt:%s%s ", sortname,
			 p->desc ? "/desc" : "/asc");
		if (p->marked)
			snprintf(marks, sizeof(marks), " marks:%zu ",
				 p->marked);
		else
			marks[0] = '\0';

		attron(hdr_attr);
		if ((int)strlen(bottom) < r->w - 4)
			mvaddnstr(r->y + r->h - 1, r->x + 1, bottom,
				  (int)strlen(bottom));
		if (marks[0] && (int)strlen(marks) < r->w - 4 -
							 (int)strlen(bottom))
			mvaddnstr(r->y + r->h - 1,
				  r->x + r->w - 1 - (int)strlen(marks), marks,
				  (int)strlen(marks));
		attroff(hdr_attr);
	}
}

/* ------------------------------------------------------------------ *
 * Incremental search                                                   *
 * ------------------------------------------------------------------ */

/*
 * Function: Move the cursor to the current incremental-search match.
 * Parameters:
 *   p (Panel *): Panel whose search buffer and row list are searched.
 * Return (void): No return value.
 */
static void search_jump(Panel *p)
{
	char needle[64];
	size_t lo = 0, hi = p->n, i;

	if (!p->search_len || p->n == 0)
		return;
	fit_into(needle, sizeof(needle), p->search);

	/* Binary search only when the rows are in plain ascending name
	 * order: descending sorts break it, and so do the dependency tree
	 * rows (they no longer follow the name order - scan linearly). */
	if (p->sort == SORT_NAME && !p->desc && p->n_tree == 0) {
		while (lo < hi) {
			size_t mid = lo + (hi - lo) / 2;

			if (strcasecmp(p->view[mid]->name, needle) < 0)
				lo = mid + 1;
			else
				hi = mid;
		}
		if (lo < p->n)
			p->cur = lo;
		return;
	}
	for (i = 0; i < p->n; i++) {
		if (strncasecmp(p->view[i]->name, needle,
				strlen(needle)) == 0) {
			p->cur = i;
			return;
		}
	}
}

/*
 * Function: Move the panel cursor by one viewport-sized page.
 * Parameters:
 *   p (Panel *): Panel to page through.
 *   dir (int): Negative for the previous page, positive for the next page.
 * Return (void): No return value.
 */
void panel_page(Panel *p, int dir)
{
	size_t page = p->rows ? p->rows : 10;

	if (p->n == 0)
		return;
	if (dir < 0) {
		if (p->cur > page)
			p->cur -= page;
		else
			p->cur = 0;
	} else {
		if (p->cur + page < p->n)
			p->cur += page;
		else
			p->cur = p->n - 1;
	}
}

/*
 * Function: Clear a panel's incremental search state.
 * Parameters:
 *   p (Panel *): Panel whose search buffer should be cleared.
 * Return (void): No return value.
 */
static void clear_search(Panel *p)
{
	p->search_len = 0;
	p->search[0] = '\0';
}

/*
 * Function: Select a row using its zero-based offset in the current viewport.
 * Parameters:
 *   p (Panel *): Panel to update.
 *   row (int): Zero-based visible row offset.
 * Return (int): 1 when a package was selected, otherwise 0.
 */
int panel_select_visible(Panel *p, int row)
{
	size_t idx;

	if (row < 0 || (size_t)row >= p->rows)
		return 0;
	idx = p->top + (size_t)row;
	if (idx >= p->n)
		return 0;
	clear_search(p);
	p->cur = idx;
	clamp_view(p);
	return 1;
}

/*
 * Function: Move the cursor by a signed number of rows for wheel input.
 * Parameters:
 *   p (Panel *): Panel to scroll.
 *   rows (int): Positive values move down; negative values move up.
 * Return (void): No return value.
 */
void panel_scroll_rows(Panel *p, int rows)
{
	size_t step;

	if (p->n == 0 || rows == 0)
		return;
	if (p->cur >= p->n)
		p->cur = p->n - 1;
	clear_search(p);
	step = rows < 0 ? (size_t)(-(long)rows) : (size_t)rows;
	if (rows < 0)
		p->cur = p->cur > step ? p->cur - step : 0;
	else if (step < p->n - p->cur)
		p->cur += step;
	else
		p->cur = p->n - 1;
	clamp_view(p);
}

/*
 * Function: Map a click on the panel scrollbar track to a viewport top.
 * Parameters:
 *   p (Panel *): Panel to scroll.
 *   row (int): Zero-based track row, where 0 is the top of the track.
 * Return (void): No return value.
 */
void panel_scroll_to_row(Panel *p, int row)
{
	size_t max_top, track;

	if (p->n == 0 || p->rows == 0 || row < 0 ||
	    (size_t)row >= p->rows)
		return;
	clear_search(p);
	max_top = p->n > p->rows ? p->n - p->rows : 0;
	track = p->rows - 1;
	p->top = track ? max_top * (size_t)row / track : 0;
	p->cur = p->top;
	clamp_view(p);
}

/*
 * Function: Apply one keyboard navigation or marking key to a panel.
 * Parameters:
 *   p (Panel *): Panel to update.
 *   key (int): ncurses key code received by the main loop.
 * Return (int): CMD_NONE for panel-local actions, or a command to dispatch.
 */
int panel_handle_key(Panel *p, int key)
{
	switch (key) {
	case KEY_UP:
		clear_search(p);
		if (p->cur > 0)
			p->cur--;
		clamp_view(p);
		return CMD_NONE;
	case KEY_DOWN:
		clear_search(p);
		if (p->n && p->cur + 1 < p->n)
			p->cur++;
		clamp_view(p);
		return CMD_NONE;
	case KEY_PPAGE:
		clear_search(p);
		panel_page(p, -1);
		clamp_view(p);
		return CMD_NONE;
	case KEY_NPAGE:
		clear_search(p);
		panel_page(p, 1);
		clamp_view(p);
		return CMD_NONE;
	case KEY_HOME:
		clear_search(p);
		p->cur = 0;
		clamp_view(p);
		return CMD_NONE;
	case KEY_END:
		clear_search(p);
		p->cur = p->n ? p->n - 1 : 0;
		clamp_view(p);
		return CMD_NONE;
	case KEY_RIGHT: /* dependency tree: expand (never switches panels) */
		clear_search(p);
		panel_tree_expand(p, p->cur);
		return CMD_NONE;
	case KEY_LEFT: /* dependency tree: collapse */
		clear_search(p);
		panel_tree_collapse(p, p->cur);
		return CMD_NONE;
	case '\n':
	case KEY_ENTER:
		panel_tree_toggle(p, p->cur);
		return CMD_NONE;
	case KEY_IC:
		panel_toggle_mark(p);
		return CMD_NONE;
	case 27: /* Esc leaves the quick search */
		clear_search(p);
		return CMD_NONE;
	case KEY_BACKSPACE:
	case 127:
	case 8:
		if (p->search_len) {
			p->search[--p->search_len] = '\0';
			if (p->search_len)
				search_jump(p);
		}
		return CMD_NONE;
	default:
		break;
	}

	if (key >= 32 && key < 256 && p->search_len + 1 < sizeof(p->search)) {
		p->search[p->search_len++] = (char)key;
		p->search[p->search_len] = '\0';
		search_jump(p);
		clamp_view(p);
	}
	return CMD_NONE;
}

/* ------------------------------------------------------------------ *
 * Marking                                                              *
 * ------------------------------------------------------------------ */

/*
 * Function: Toggle the mark on the current package and advance.
 * Parameters:
 *   p (Panel *): Panel whose current package is toggled.
 * Return (void): No return value.
 */
void panel_toggle_mark(Panel *p)
{
	Package *pk = panel_current(p);

	if (!pk)
		return;
	if (pk->flags & PKGF_MARKED) {
		pk->flags &= (unsigned char)~PKGF_MARKED;
		if (p->marked)
			p->marked--;
	} else {
		pk->flags |= PKGF_MARKED;
		p->marked++;
	}
	if (p->cur + 1 < p->n)
		p->cur++;
	clamp_view(p);
}

/*
 * Function: Mark or unmark packages matching a glob pattern.
 * Parameters:
 *   p (Panel *): Panel whose packages are filtered.
 *   pattern (const char *): fnmatch glob pattern.
 *   mark (int): Non-zero marks matches; zero clears matching marks.
 * Return (void): No return value.
 */
void panel_mark_pattern(Panel *p, const char *pattern, int mark)
{
	size_t i;

	if (!pattern || !*pattern)
		return;
	for (i = 0; i < p->n; i++) {
		Package *pk = p->view[i];

		if (fnmatch(pattern, pk->name, 0) != 0)
			continue;
		if (mark && !(pk->flags & PKGF_MARKED)) {
			pk->flags |= PKGF_MARKED;
			p->marked++;
		} else if (!mark && (pk->flags & PKGF_MARKED)) {
			pk->flags &= (unsigned char)~PKGF_MARKED;
			if (p->marked)
				p->marked--;
		}
	}
}

/*
 * Function: Clear all package marks in a panel.
 * Parameters:
 *   p (Panel *): Panel whose marks should be cleared.
 * Return (void): No return value.
 */
void panel_clear_marks(Panel *p)
{
	size_t i;

	for (i = 0; i < p->n; i++)
		p->view[i]->flags &= (unsigned char)~PKGF_MARKED;
	p->marked = 0;
}

/*
 * Function: Find the next package whose name contains a substring.
 * Parameters:
 *   p (Panel *): Panel to search.
 *   needle (const char *): Case-insensitive substring to find.
 * Return (int): 1 when a package was selected, otherwise 0.
 */
int panel_find_sub(Panel *p, const char *needle)
{
	size_t i;

	if (!needle || !*needle || p->n == 0)
		return 0;
	for (i = p->cur + 1; i < p->n; i++) {
		if (strcasestr(p->view[i]->name, needle)) {
			p->cur = i;
			clamp_view(p);
			return 1;
		}
	}
	for (i = 0; i < p->n; i++) {
		if (strcasestr(p->view[i]->name, needle)) {
			p->cur = i;
			clamp_view(p);
			return 1;
		}
	}
	return 0;
}
