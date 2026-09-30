/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 APPIT Adam Skowroński
 */
#include "pkg.h"
#include "proc.h"
#include "util.h"
#include "vercmp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ *
 * Error reporting                                                     *
 * ------------------------------------------------------------------ */

static char g_err[512];

/* Function: Return the most recent package-database error message.
 * Parameters: None.
 * Return (const char *): Borrowed static text that is never NULL; "unknown error" when no error has been recorded.
 */
const char *pkgdb_error(void)
{
	return g_err[0] ? g_err : "unknown error";
}

/* Function: Record a package-database error in shared storage, replacing any prior text. Messages longer than the 512-byte storage are truncated.
 * Parameters:
 *   fmt (const char *): Non-NULL printf-style format string.
 *   ... (...): Values consumed by fmt.
 * Return (void): No return value.
 */
static void set_error(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(g_err, sizeof(g_err), fmt, ap);
	va_end(ap);
}

/* ------------------------------------------------------------------ *
 * Name hash: open addressing, FNV-1a, stores index + 1 (0 = empty).   *
 * The table is kept up to date by pkgdb_add(), so lookups work at any *
 * point of the load.                                                  *
 * ------------------------------------------------------------------ */

typedef struct {
	size_t cap;
	int slots[];
} IdxHdr;

/* Function: Compute the FNV-1a hash used by the package-name index.
 * Parameters:
 *   s (const char *): Non-NULL NUL-terminated package name.
 * Return (size_t): Hash value suitable for masking with the index capacity minus one.
 */
static size_t hash_name(const char *s)
{
	size_t h = (size_t)1469598103934665603ULL;

	while (*s) {
		h ^= (unsigned char)*s++;
		h *= (size_t)1099511628211ULL;
	}
	return h;
}

/* Function: Round a load count up to a power of two.
 * Parameters:
 *   n (size_t): Count to round; it must fit in the largest power of two representable by size_t.
 * Return (size_t): The smallest power of two at least n, with a minimum of 16.
 */
static size_t next_pow2(size_t n)
{
	size_t p = 16;

	while (p < n)
		p <<= 1;
	return p;
}

/* Function: Insert one package name into an existing hash table by linear probing.
 * Parameters:
 *   db (PkgDB *): Non-NULL database with a non-NULL index that has spare capacity.
 *   i (size_t): Valid package index in the range 0 through db->n - 1; the name must not already be present in the index.
 * Return (void): No return value.
 */
static void index_insert(PkgDB *db, size_t i)
{
	IdxHdr *h = db->index;
	size_t mask, slot;

	if (!h)
		return;
	mask = h->cap - 1;
	slot = hash_name(db->v[i].name) & mask;
	while (h->slots[slot])
		slot = (slot + 1) & mask;
	h->slots[slot] = (int)i + 1;
}

/* Function: Rebuild the package-name index at no more than 50 percent load.
 * Parameters:
 *   db (PkgDB *): Non-NULL initialized database. Any existing index is released; package storage is unchanged.
 * Return (void): No return value.
 */
static void index_build(PkgDB *db)
{
	IdxHdr *h;
	size_t cap, i;

	free(db->index);
	db->index = NULL;
	if (db->n == 0)
		return;

	/* index_build() sizes the table for a load factor of 0.5. */
	cap = next_pow2(db->n * 2);
	h = xcalloc(1, sizeof(IdxHdr) + cap * sizeof(int));
	h->cap = cap;
	db->index = h;
	for (i = 0; i < db->n; i++)
		index_insert(db, i);
}

/* Function: Register a just-appended package, rebuilding when index load reaches 75 percent.
 * Parameters:
 *   db (PkgDB *): Non-NULL initialized database; its index may be NULL.
 *   i (size_t): Newly appended package index in the range 0 through db->n - 1.
 * Return (void): No return value.
 */
static void index_touch(PkgDB *db, size_t i)
{
	IdxHdr *h = db->index;

	if (!h) {
		index_build(db);
		return;
	}
	if (db->n >= h->cap - h->cap / 4) {
		index_build(db);
		return;
	}
	index_insert(db, i);
}

/* Function: Find a package by its exact name through the current name index.
 * Parameters:
 *   db (PkgDB *): Non-NULL initialized database; a NULL index yields no match.
 *   name (const char *): NUL-terminated exact package name; NULL is tolerated and yields no match.
 * Return (Package *): Borrowed pointer into db, or NULL when absent. Adding a package, clearing the database, or otherwise reallocating db->v invalidates it.
 */
Package *pkgdb_find(PkgDB *db, const char *name)
{
	IdxHdr *h = db->index;
	size_t mask, slot, guard;

	if (!h || !name)
		return NULL;
	mask = h->cap - 1;
	slot = hash_name(name) & mask;
	guard = h->cap;
	while (h->slots[slot] && guard--) {
		Package *p = &db->v[h->slots[slot] - 1];

		if (strcmp(p->name, name) == 0)
			return p;
		slot = (slot + 1) & mask;
	}
	return NULL;
}

/* Function: Return the package stored at a validated vector index.
 * Parameters:
 *   db (PkgDB *): Initialized database to query; NULL is tolerated and yields no result.
 *   i (size_t): Zero-based index that must be less than db->n.
 * Return (Package *): Borrowed pointer into db, or NULL for a NULL database or an out-of-range index. Database reallocation invalidates it.
 */
Package *pkgdb_at(PkgDB *db, size_t i)
{
	if (!db || i >= db->n)
		return NULL;
	return &db->v[i];
}

/* Function: Append a uniquely named package and keep the name index current.
 * Parameters:
 *   db (PkgDB *): Non-NULL initialized database; name must not already exist.
 *   name (const char *): Non-NULL NUL-terminated package name to duplicate into the entry.
 * Return (Package *): Borrowed pointer to the new database entry. Do not free it directly; a vector reallocation invalidates all prior Package pointers.
 */
static Package *pkgdb_add(PkgDB *db, const char *name)
{
	Package *p;

	if (db->n == db->cap) {
		db->cap = db->cap ? db->cap * 2 : 256;
		db->v = xrealloc(db->v, db->cap * sizeof(*db->v));
	}
	p = &db->v[db->n];
	memset(p, 0, sizeof(*p));
	p->name = xstrdup(name);
	db->n++;
	index_touch(db, db->n - 1);
	return p;
}

/* Function: Release all heap-backed fields of one package and zero its contents.
 * Parameters:
 *   p (Package *): Non-NULL package element; the containing storage is not freed.
 * Return (void): No return value.
 */
static void pkg_free_fields(Package *p)
{
	size_t i;

	free(p->name);
	free(p->installed);
	free(p->candidate);
	free(p->repo);
	free(p->arch);
	for (i = 0; i < p->n_deps; i++)
		free(p->deps[i]);
	free(p->deps);
	memset(p, 0, sizeof(*p));
}

/* Function: Prepare a caller-owned package database as empty.
 * Parameters:
 *   db (PkgDB *): Non-NULL uninitialized storage; it must not own prior allocations.
 * Return (void): No return value.
 */
void pkgdb_init(PkgDB *db)
{
	db->v = NULL;
	db->n = db->cap = 0;
	db->index = NULL;
}

/* Function: Release all packages and index storage, then reset the database to empty.
 * Parameters:
 *   db (PkgDB *): Non-NULL database initialized by pkgdb_init() or already cleared. Every prior Package pointer becomes invalid.
 * Return (void): No return value.
 */
void pkgdb_clear(PkgDB *db)
{
	size_t i;

	for (i = 0; i < db->n; i++)
		pkg_free_fields(&db->v[i]);
	free(db->v);
	db->v = NULL;
	db->n = db->cap = 0;
	free(db->index);
	db->index = NULL;
}

/* Function: Destroy a package database through pkgdb_clear(), leaving it reusable.
 * Parameters:
 *   db (PkgDB *): Non-NULL initialized database; every prior Package pointer becomes invalid.
 * Return (void): No return value.
 */
void pkgdb_free(PkgDB *db)
{
	pkgdb_clear(db);
}

/* Function: Clear PKGF_MARKED on every current package without changing other state.
 * Parameters:
 *   db (PkgDB *): Non-NULL initialized database.
 * Return (void): No return value.
 */
void pkgdb_unmark_all(PkgDB *db)
{
	size_t i;

	for (i = 0; i < db->n; i++)
		db->v[i].flags &= (unsigned char)~PKGF_MARKED;
}

/* ------------------------------------------------------------------ *
 * "apt-cache policy" parsing                                          *
 * ------------------------------------------------------------------ */

/*
 * Output shape (LC_ALL=C):
 *
 *   bash:
 *     Installed: 5.2.21-2ubuntu3
 *     Candidate: 5.2.21-2ubuntu3
 *     Version table:
 *    *** 5.2.21-2ubuntu3 500
 *          500 http://archive.ubuntu.com/ubuntu noble/main amd64 Packages
 *       5.2.21-2ubuntu1 500
 *          500 http://...
 */

typedef struct {
	PkgDB *db;
	LoadProgress cb;
	void *ud;
	Buf err;
	int cur;         /* index of the package being parsed, -1 */
	int in_table;
	int want_repo;   /* the parsed entry is the candidate version */
	int no_add;      /* never grow the database (panels hold pointers) */
	char *candidate; /* candidate of the current package */
	char *pending;   /* version of the entry being read */
	size_t count;
} PolicyCtx;

/* Function: Install a trimmed string in a package field, mapping apt's "(none)" to NULL.
 * Parameters:
 *   dst (char **): Address of a NULL or heap-owned string field; its old value is replaced.
 *   v (char *): NULL for no change; otherwise a distinct mutable heap string whose ownership transfers to *dst unless it equals "(none)", when it is freed.
 * Return (void): No return value.
 */
static void set_str(char **dst, char *v)
{
	if (!v)
		return;
	str_trim(v);
	if (strcmp(v, "(none)") == 0) {
		free(v);
		*dst = NULL;
		return;
	}
	free(*dst);
	*dst = v;
}

/* Function: Extract a compact repository label from an apt-cache version-table origin. HTTP(S) schemes are omitted; a local path is rendered as "(local)".
 * Parameters:
 *   line (const char *): Non-NULL NUL-terminated origin line; input is truncated to fit the 512-byte internal buffer.
 * Return (char *): Newly allocated display label owned by the caller, or NULL when the line has fewer than two tokens.
 */
static char *repo_of_origin(const char *line)
{
	char buf[512];
	char *tok[6];
	int n = 0;
	char *save = NULL;
	char *p;
	const char *url;

	fit_into(buf, sizeof(buf), line);
	p = strtok_r(buf, " \t", &save);
	while (p && n < 6) {
		tok[n++] = p;
		p = strtok_r(NULL, " \t", &save);
	}
	if (n < 2)
		return NULL;
	if (tok[1][0] == '/')
		return xstrdup("(local)");
	url = tok[1];
	if (str_starts(url, "http://"))
		url += 7;
	else if (str_starts(url, "https://"))
		url += 8;
	if (n >= 3)
		return xasprintf("%s %s", url, tok[2]);
	return xstrdup(url);
}

/* Function: Consume one apt-cache policy line and update parsing context and package data.
 * Parameters:
 *   c (PolicyCtx *): Non-NULL initialized PolicyCtx.
 *   line (const char *): NUL-terminated temporary input line; blank and unrecognized lines are ignored.
 * Return (void): No return value.
 */
static void policy_line(PolicyCtx *c, const char *line)
{
	Package *p;
	size_t len;

	if (!line[0])
		return;

	/* Package header: "name:" at column 0. */
	if (line[0] != ' ') {
		len = strlen(line);
		if (len < 2 || line[len - 1] != ':')
			return; /* error chatter, ignore */
		c->cur = -1;
		{
			char *name = xstrndup(line, len - 1);
			Package *ex = pkgdb_find(c->db, name);

			if (ex) {
				free(name);
				c->cur = (int)(ex - c->db->v);
			} else if (c->no_add) {
				/* Growing the database here would move it
				 * under the panels holding pointers into
				 * it; ignore what we did not ask for. */
				free(name);
				c->cur = -1;
			} else {
				c->cur = (int)c->db->n;
				pkgdb_add(c->db, name);
				free(name);
			}
		}
		c->in_table = 0;
		free(c->candidate);
		c->candidate = NULL;
		free(c->pending);
		c->pending = NULL;
		c->want_repo = 0;
		c->count++;
		if (c->cb && (c->count % 10000) == 0) {
			char msg[96];

			snprintf(msg, sizeof(msg),
				 "Reading package lists... %zu packages",
				 c->count);
			c->cb(c->ud, msg);
		}
		return;
	}

	if (c->cur < 0)
		return;
	p = &c->db->v[c->cur];

	if (str_starts(line, "  Installed:")) {
		char *v = xstrdup(line + strlen("  Installed:"));

		set_str(&p->installed, v);
		return;
	}
	if (str_starts(line, "  Candidate:")) {
		char *v = xstrdup(line + strlen("  Candidate:"));

		str_trim(v);
		if (strcmp(v, "(none)") == 0) {
			free(v);
			v = NULL;
		}
		free(c->candidate);
		c->candidate = v ? xstrdup(v) : NULL;
		/* For an installed package apt knows no better version than
		 * what we already got from dpkg; keep ours when apt reports
		 * no candidate at all. */
		if (v || !(p->flags & PKGF_INSTALLED)) {
			free(p->candidate);
			p->candidate = v;
		} else {
			free(v);
		}
		return;
	}
	if (str_starts(line, "  Version table:")) {
		c->in_table = 1;
		return;
	}
	if (!c->in_table)
		return;

	/* Version entry (5 spaces) or origin line (8 spaces). */
	{
		int indent = 0;
		const char *q = line;

		while (*q == ' ') {
			indent++;
			q++;
		}
		if (!*q)
			return;

		if (indent <= 5) {
			/* "<version> <priority>", maybe prefixed with "***" */
			char buf[256];
			char *save = NULL;
			char *tok;

			fit_into(buf, sizeof(buf), q);
			tok = strtok_r(buf, " \t", &save);
			if (tok && strcmp(tok, "***") == 0)
				tok = strtok_r(NULL, " \t", &save);
			free(c->pending);
			c->pending = tok ? xstrdup(tok) : NULL;
			c->want_repo = (c->pending && c->candidate &&
					strcmp(c->pending, c->candidate) == 0);
		} else if (c->want_repo) {
			char *r = repo_of_origin(q);

			if (r) {
				free(p->repo);
				p->repo = r;
			}
			c->want_repo = 0;
		}
	}
}

/* Function: Route proc_run() output into the apt-cache policy parser and error buffer.
 * Parameters:
 *   ud (void *): Cast to a PolicyCtx *, supplied unchanged by proc_run().
 *   line (const char *): NUL-terminated line valid only for the duration of this call.
 *   is_err (int): Nonzero captures standard error instead of parsing standard output.
 * Return (void): No return value.
 */
static void policy_cb(void *ud, const char *line, int is_err)
{
	PolicyCtx *c = ud;

	if (is_err) {
		buf_puts(&c->err, line);
		buf_putc(&c->err, '\n');
		return;
	}
	policy_line(c, line);
}

/* ------------------------------------------------------------------ *
 * Running "apt-cache policy" over a list of names.                    *
 *                                                                      *
 * The whole cache would need ~87000 arguments and takes about 25       *
 * seconds, so this is only ever called with a bounded set: either the *
 * installed packages at load time, or the rows of the visible panels  *
 * later on.  Long lists are split into chunks that stay well below    *
 * ARG_MAX.                                                             *
 * ------------------------------------------------------------------ */

#define POLICY_MAX_ARGS  8000
#define POLICY_MAX_BYTES 400000

/* Function: Run one bounded apt-cache policy command after resetting per-chunk state.
 * Parameters:
 *   c (PolicyCtx *): Non-NULL initialized PolicyCtx.
 *   argv (char *const []): Non-NULL, NULL-terminated apt-cache argument vector; its strings are borrowed.
 *   what (const char *): Non-NULL borrowed operation label used in any error message.
 * Return (int): 0 when apt-cache exits successfully, otherwise -1 after recording the failure through set_error().
 */
static int policy_chunk(PolicyCtx *c, char *const argv[], const char *what)
{
	int status;

	c->cur = -1;
	c->in_table = 0;
	c->want_repo = 0;
	free(c->candidate);
	c->candidate = NULL;
	free(c->pending);
	c->pending = NULL;
	buf_reset(&c->err);

	status = proc_run(argv, NULL, policy_cb, NULL, c);
	if (status != 0) {
		char msg[256];
		char *first = c->err.data;

		if (first) {
			char *nl = strchr(first, '\n');

			if (nl)
				*nl = '\0';
		}
		fit_into(msg, sizeof(msg), first && *first ? first :
							       "no output");
		set_error("%s failed (exit %d): %s", what, status, msg);
		return -1;
	}
	return 0;
}

/* Function: Run apt-cache policy for a list of names in argument- and byte-bounded chunks.
 * Parameters:
 *   db (PkgDB *): Non-NULL initialized database to update.
 *   cb (LoadProgress): Optional progress callback; NULL disables periodic messages.
 *   ud (void *): Opaque value passed unchanged to cb.
 *   names (char *const []): Array of n borrowed, non-NULL NUL-terminated package names; NULL is accepted only when n is zero.
 *   n (size_t): Number of names; zero is a successful no-op.
 *   what (const char *): Non-NULL borrowed operation label used in errors.
 *   no_add (int): Nonzero prevents unknown package names from growing db.
 * Return (int): 0 after all chunks succeed, or -1 on the first command failure.
 */
static int policy_names(PkgDB *db, LoadProgress cb, void *ud,
			char *const names[], size_t n, const char *what,
			int no_add)
{
	PolicyCtx c;
	char **argv;
	size_t k;
	size_t bytes = 0;
	int nargs = 2;
	int rc = 0;

	if (!names || n == 0)
		return 0;

	argv = xmalloc((POLICY_MAX_ARGS + 3) * sizeof(*argv));
	argv[0] = "apt-cache";
	argv[1] = "policy";

	memset(&c, 0, sizeof(c));
	c.db = db;
	c.cb = cb;
	c.ud = ud;
	c.cur = -1;
	c.no_add = no_add;
	buf_init(&c.err);

	for (k = 0; k < n && rc == 0; k++) {
		size_t len = strlen(names[k]) + 1;

		if (nargs > 2 && (nargs >= POLICY_MAX_ARGS + 2 ||
				  bytes + len > POLICY_MAX_BYTES)) {
			argv[nargs] = NULL;
			rc = policy_chunk(&c, argv, what);
			nargs = 2;
			bytes = 0;
		}
		argv[nargs++] = names[k];
		bytes += len;
	}
	if (rc == 0 && nargs > 2) {
		argv[nargs] = NULL;
		rc = policy_chunk(&c, argv, what);
	}

	free(c.candidate);
	free(c.pending);
	buf_free(&c.err);
	free(argv);
	return rc;
}

/* Function: Refresh exact candidate origins for requested packages and cache completion.
 * Parameters:
 *   db (PkgDB *): Non-NULL initialized database.
 *   names (char *const []): Array of n borrowed, non-NULL package-name strings; NULL is accepted only when n is zero.
 *   n (size_t): Number of requested names.
 * Return (int): 0 on success, including a zero-length request; on success every found requested package is marked PKGF_ORIGIN. Returns -1 on command failure.
 */
int pkgdb_fetch_origins(PkgDB *db, char *const names[], size_t n)
{
	size_t i;

	if (policy_names(db, NULL, NULL, names, n, "apt-cache policy", 1) !=
	    0)
		return -1;

	/* Whether an origin was found or not, the question was answered:
	 * never ask about these packages again. */
	for (i = 0; i < n; i++) {
		Package *p = pkgdb_find(db, names[i]);

		if (p)
			p->flags |= PKGF_ORIGIN;
	}
	return 0;
}

/* ------------------------------------------------------------------ *
 * "apt-cache depends" parsing                                         *
 *                                                                      *
 * Output shape (LC_ALL=C, alternatives stripped):                     *
 *                                                                      *
 *   7zip:                                                              *
 *     Depends: libc6                                                  *
 *     Depends: libgcc-s1                                              *
 *   accountsservice:                                                  *
 *    |Depends: <default-dbus-system-bus>      (virtual, line 2 alt.)  *
 *       dbus                                   (providers, 4 spaces)   *
 *     Depends: <dbus-system-bus>                                       *
 *       dbus-broker                                                       *
 *     Depends: libc6                                                  *
 *                                                                      *
 * Column 0 starts a new package block.  "Depends:"/"PreDepends:"      *
 * relations live on 2 space indented lines (a leading " |" marks an    *
 * alternative branch).  A "<...>" value is a virtual package: the real *
 * providers follow on 4 space indented lines.                          *
 * ------------------------------------------------------------------ */

typedef struct {
	PkgDB *db;
	Package *cur;        /* package block being parsed, may be NULL */
	int pending_virtual; /* previous relation was virtual */
} DepsCtx;

/* Function: Resolve a dependency span to a canonical database name, retrying without an architecture qualifier when needed.
 * Parameters:
 *   db (PkgDB *): Non-NULL initialized package database.
 *   name (const char *): Borrowed byte span that need not be NUL-terminated.
 *   len (size_t): Dependency length from 1 through 127; zero or larger values are rejected.
 * Return (const char *): Borrowed canonical name valid until db changes, or NULL when the dependency cannot be resolved.
 */
static const char *deps_resolve(PkgDB *db, const char *name, size_t len)
{
	char buf[128];
	Package *p;

	if (len == 0 || len >= sizeof(buf))
		return NULL;
	memcpy(buf, name, len);
	buf[len] = '\0';
	p = pkgdb_find(db, buf);
	if (!p) {
		char *colon = strrchr(buf, ':');

		if (colon && colon != buf) {
			*colon = '\0';
			p = pkgdb_find(db, buf);
		}
	}
	return p ? p->name : NULL;
}

/* Function: Resolve a child-name span and append its canonical name once to the current package.
 * Parameters:
 *   c (DepsCtx *): Non-NULL DepsCtx whose cur field may identify a database package.
 *   name (const char *): Borrowed child-name span.
 *   len (size_t): Span length in the range accepted by deps_resolve().
 * Return (void): No return value.
 */
static void deps_add_resolved(DepsCtx *c, const char *name, size_t len)
{
	const char *canon;
	Package *p;
	size_t i;

	if (!c->cur)
		return;
	canon = deps_resolve(c->db, name, len);
	if (!canon)
		return;
	for (i = 0; i < c->cur->n_deps; i++)
		if (strcmp(c->cur->deps[i], canon) == 0)
			return;
	p = c->cur;
	p->deps = xrealloc(p->deps, (p->n_deps + 1) * sizeof(*p->deps));
	p->deps[p->n_deps++] = xstrdup(canon);
}

/* Function: Parse one apt-cache depends line and update the current dependency state.
 * Parameters:
 *   ud (void *): Cast to a DepsCtx *, supplied unchanged by proc_run().
 *   line (const char *): NUL-terminated temporary input line.
 *   is_err (int): Nonzero ignores the line as standard-error output.
 * Return (void): No return value.
 */
static void deps_line(void *ud, const char *line, int is_err)
{
	DepsCtx *c = ud;
	const char *q;
	size_t indent, len;

	if (is_err || !line[0])
		return;

	/* Column 0: a new package block. */
	if (line[0] != ' ' && line[0] != '|') {
		Package *p = pkgdb_find(c->db, line);

		/* Already fetched (duplicate argument): skip the block. */
		c->cur = (p && !(p->flags & PKGF_DEPS)) ? p : NULL;
		c->pending_virtual = 0;
		return;
	}
	if (!c->cur)
		return;

	indent = strspn(line, " ");
	q = line + indent;
	if (*q == '|') { /* alternative branch: " |Depends: ..." */
		q++;
		q += strspn(q, " ");
		indent = 0; /* it is a relation line, not a provider */
	}
	if (indent >= 4) { /* provider of the previous virtual relation */
		if (c->pending_virtual) {
			len = strcspn(q, " \t(");
			deps_add_resolved(c, q, len);
		}
		return;
	}
	if (strncmp(q, "PreDepends:", 11) == 0)
		q += 11;
	else if (strncmp(q, "Depends:", 8) == 0)
		q += 8;
	else
		return;
	q += strspn(q, " \t");
	if (*q == '<') { /* virtual: only the providers matter */
		c->pending_virtual = 1;
		return;
	}
	c->pending_virtual = 0;
	len = strcspn(q, " \t(<");
	deps_add_resolved(c, q, len);
}

/* ------------------------------------------------------------------ *
 * Running "apt-cache depends" over a list of names.                   *
 * ------------------------------------------------------------------ */

#define DEPS_MAX_ARGS  800
#define DEPS_MAX_BYTES 40000

/* Only Depends/PreDepends are wanted for the tree, everything else
 * (Recommends, Conflicts, ...) is switched off. */
static const char *const deps_flags[] = {
	"--no-recommends",	     "--no-suggests",
	"--no-conflicts",	     "--no-breaks",
	"--no-replaces",	     "--no-enhances",
};
#define DEPS_NFLAGS ((int)(sizeof(deps_flags) / sizeof(deps_flags[0])))

/* Function: Run one bounded apt-cache depends command after resetting parser state.
 * Parameters:
 *   c (DepsCtx *): Non-NULL initialized DepsCtx.
 *   argv (char *const []): Non-NULL, NULL-terminated apt-cache argument vector; its strings are borrowed.
 * Return (int): 0 when apt-cache exits successfully, otherwise -1 after recording the failure through set_error().
 */
static int deps_chunk(DepsCtx *c, char *const argv[])
{
	int status;

	c->cur = NULL;
	c->pending_virtual = 0;
	status = proc_run(argv, NULL, deps_line, NULL, c);
	if (status != 0) {
		set_error("apt-cache depends failed (exit %d)", status);
		return -1;
	}
	return 0;
}

/* Function: Fetch and cache direct Depends and PreDepends names for requested packages.
 * Parameters:
 *   db (PkgDB *): Non-NULL initialized database.
 *   names (char *const []): Array of n borrowed, non-NULL package-name strings; NULL is accepted only when n is zero.
 *   n (size_t): Number of requested names.
 * Return (int): 0 on success, including a zero-length request; on success every found requested package is marked PKGF_DEPS. Returns -1 on command failure.
 */
int pkgdb_fetch_deps(PkgDB *db, char *const names[], size_t n)
{
	DepsCtx c;
	char **argv;
	size_t k;
	size_t bytes = 0;
	int nargs = 2 + DEPS_NFLAGS;
	int rc = 0;

	if (!names || n == 0)
		return 0;

	argv = xmalloc((DEPS_MAX_ARGS + DEPS_NFLAGS + 3) * sizeof(*argv));
	argv[0] = "apt-cache";
	argv[1] = "depends";
	for (k = 0; (int)k < DEPS_NFLAGS; k++)
		argv[2 + k] = (char *)deps_flags[k];

	memset(&c, 0, sizeof(c));
	c.db = db;

	for (k = 0; k < n && rc == 0; k++) {
		size_t len = strlen(names[k]) + 1;

		if (nargs > 2 + DEPS_NFLAGS &&
		    (nargs >= DEPS_MAX_ARGS + DEPS_NFLAGS ||
		     bytes + len > DEPS_MAX_BYTES)) {
			argv[nargs] = NULL;
			rc = deps_chunk(&c, argv);
			nargs = 2 + DEPS_NFLAGS;
			bytes = 0;
		}
		argv[nargs++] = names[k];
		bytes += len;
	}
	if (rc == 0 && nargs > 2 + DEPS_NFLAGS) {
		argv[nargs] = NULL;
		rc = deps_chunk(&c, argv);
	}
	free(argv);
	if (rc != 0)
		return -1;

	/* Whether dependencies were found or not, the question was
	 * answered: never ask about these packages again. */
	for (k = 0; k < n; k++) {
		Package *p = pkgdb_find(db, names[k]);

		if (p)
			p->flags |= PKGF_DEPS;
	}
	return 0;
}

/* ------------------------------------------------------------------ *
 * "dpkg-query -W" parsing                                              *
 * ------------------------------------------------------------------ */

typedef struct {
	PkgDB *db;
	LoadProgress cb;
	void *ud;
} DpkgCtx;

/* Function: Parse one dpkg-query output line and merge installed package metadata.
 * Parameters:
 *   ud (void *): Cast to a DpkgCtx *, supplied unchanged by proc_run().
 *   line (const char *): NUL-terminated temporary input line.
 *   is_err (int): Ignored; standard-error lines are parsed like any other line.
 * Return (void): No return value.
 */
static void dpkg_cb(void *ud, const char *line, int is_err)
{
	DpkgCtx *c = ud;
	char *copy, *f[5];
	int i, got;
	Package *p;

	(void)is_err;
	if (!line[0])
		return;

	copy = xstrdup(line);
	for (i = 0; i < 5; i++)
		f[i] = NULL;
	{
		char *save = NULL;
		char *tok = strtok_r(copy, "\t", &save);

		for (got = 0; tok && got < 5; got++) {
			f[got] = tok;
			tok = strtok_r(NULL, "\t", &save);
		}
	}
	if (got < 4 || !f[0]) {
		free(copy);
		return;
	}
	/* ${db:Status-Abbrev} is three characters; "ii " means installed. */
	if (!(f[3][0] == 'i' && f[3][1] == 'i')) {
		free(copy);
		return;
	}

	p = pkgdb_find(c->db, f[0]);
	if (!p) {
		p = pkgdb_add(c->db, f[0]);
	} else if (p->flags & PKGF_INSTALLED) {
		/* The same package installed for a second architecture
		 * (libc6:amd64 plus libc6:i386).  Prefer the native arch
		 * and add up the disk usage. */
		if (f[2] && f[2][0] && (!p->arch ||
					 (strcmp(p->arch, "amd64") != 0 &&
					  strcmp(f[2], "amd64") == 0))) {
			free(p->arch);
			p->arch = xstrdup(f[2]);
			if (f[1] && f[1][0]) {
				free(p->installed);
				p->installed = xstrdup(f[1]);
			}
		}
		if (f[4] && f[4][0])
			p->size_kib += strtoul(f[4], NULL, 10);
		free(copy);
		return;
	}
	p->flags |= PKGF_INSTALLED;
	if (f[1] && f[1][0]) {
		free(p->installed);
		p->installed = xstrdup(f[1]);
	}
	if (f[2] && f[2][0]) {
		free(p->arch);
		p->arch = xstrdup(f[2]);
	}
	if (f[4] && f[4][0])
		p->size_kib = strtoul(f[4], NULL, 10);
	free(copy);
}

/* ------------------------------------------------------------------ *
 * "apt list" parsing                                                   *
 * ------------------------------------------------------------------ */

/*
 * Output shape (LC_ALL=C), one line per package, release and arch:
 *
 *   Listing...
 *   bash/noble,now 5.2.21-2ubuntu4 amd64 [installed]
 *   bash/noble 5.2.21-2ubuntu4 i386
 *   0ad/noble-updates 0.0.26-6ubuntu0.24.04.1 amd64
 *
 * It is the cheapest way to learn the candidate version of every
 * package in the cache (about two seconds for ~92000 lines).  The
 * releases after the slash are only a rough origin until
 * "apt-cache policy" refines them for the rows actually on screen.
 */

#define REPO_MAX 96

typedef struct {
	PkgDB *db;
	LoadProgress cb;
	void *ud;
	Buf err;
	size_t count;
} AptListCtx;

/* Function: Score an apt-list architecture so a better candidate can replace a worse one.
 * Parameters:
 *   p (const Package *): Non-NULL package whose current architecture and flags are inspected.
 *   arch (const char *): Non-NULL NUL-terminated architecture token.
 * Return (int): 3 for an installed package's matching architecture or preferred "amd64", 2 for uninstalled "all", or 1 as a fallback.
 */
static int arch_rank(const Package *p, const char *arch)
{
	if (p->flags & PKGF_INSTALLED)
		return (p->arch && strcmp(p->arch, arch) == 0) ? 3 : 1;
	if (strcmp(arch, "amd64") == 0)
		return 3;
	if (strcmp(arch, "all") == 0)
		return 2;
	return 1;
}

/* Function: Test for an exact item in a comma-separated repository list.
 * Parameters:
 *   repo (const char *): Non-NULL NUL-terminated comma-separated list.
 *   item (const char *): Non-NULL NUL-terminated token to match; whitespace is not trimmed.
 * Return (int): 1 for an exact list element, otherwise 0.
 */
static int repo_has(const char *repo, const char *item)
{
	const char *p = repo;
	size_t l = strlen(item);

	while (*p) {
		const char *e = strchr(p, ',');
		size_t n = e ? (size_t)(e - p) : strlen(p);

		if (n == l && strncmp(p, item, l) == 0)
			return 1;
		if (!e)
			break;
		p = e + 1;
	}
	return 0;
}

/* Function: Merge comma-separated releases into a bounded, deduplicated repository list. Empty tokens and the installed-only "now" marker are discarded.
 * Parameters:
 *   dst (char **): Address of a NULL or heap-owned repository string; its allocation remains owned by the caller.
 *   suites (const char *): Borrowed NUL-terminated comma-separated input; NULL or empty is a no-op. Input is truncated to 255 bytes and output to 95.
 * Return (void): No return value.
 */
static void suites_merge(char **dst, const char *suites)
{
	char buf[256], *save = NULL, *tok;

	if (!suites || !*suites)
		return;
	fit_into(buf, sizeof(buf), suites);
	for (tok = strtok_r(buf, ",", &save); tok;
	     tok = strtok_r(NULL, ",", &save)) {
		size_t have = *dst ? strlen(*dst) : 0;
		size_t l = strlen(tok);

		if (!l || strcmp(tok, "now") == 0)
			continue;
		if (*dst && repo_has(*dst, tok))
			continue;
		if (have + l + (have ? 1 : 0) + 1 > REPO_MAX)
			break;
		if (!*dst)
			*dst = xstrdup(tok);
		else {
			*dst = xrealloc(*dst, have + l + 2);
			(*dst)[have] = ',';
			memcpy(*dst + have + 1, tok, l + 1);
		}
	}
}

/* Function: Parse one apt list entry and update package candidate, origin, and flags. Header and malformed lines are ignored.
 * Parameters:
 *   c (AptListCtx *): Non-NULL initialized AptListCtx.
 *   line (const char *): NUL-terminated temporary input line.
 * Return (void): No return value.
 */
static void aptlist_line(AptListCtx *c, const char *line)
{
	char buf[1024];
	char flags[256];
	char *t[16];
	int n = 0, i;
	size_t off = 0;
	char *save = NULL;
	char *slash;
	const char *ver, *arch;
	Package *p;
	int rank, cur;

	if (!line[0])
		return;
	fit_into(buf, sizeof(buf), line);
	{
		char *tok = strtok_r(buf, " \t", &save);

		while (tok && n < 16) {
			t[n++] = tok;
			tok = strtok_r(NULL, " \t", &save);
		}
	}
	if (n < 3)
		return;
	slash = strchr(t[0], '/');
	if (!slash || slash == t[0])
		return; /* the "Listing..." header and friends */
	*slash = '\0';
	ver = t[1];
	arch = t[2];

	/* The flags may contain spaces: "[upgradable from: 1.2-3]". */
	for (i = 3; i < n && off + 2 < sizeof(flags); i++) {
		size_t l = strlen(t[i]);

		if (off) {
			if (off + 1 >= sizeof(flags))
				break;
			flags[off++] = ' ';
		}
		if (off + l >= sizeof(flags))
			l = sizeof(flags) - off - 1;
		memcpy(flags + off, t[i], l);
		off += l;
	}
	flags[off] = '\0';

	p = pkgdb_find(c->db, t[0]);
	if (!p)
		p = pkgdb_add(c->db, t[0]);

	rank = arch_rank(p, arch);
	cur = p->arch ? arch_rank(p, p->arch) : 0;

	if (!p->candidate) {
		p->candidate = xstrdup(ver);
		if (!p->arch)
			p->arch = xstrdup(arch);
	} else if (rank > cur) {
		/* A better architecture than the one recorded so far.
		 * For installed packages the version decided by
		 * "apt-cache policy" wins and never gets replaced. */
		if (strcmp(ver, p->candidate) != 0) {
			free(p->candidate);
			p->candidate = xstrdup(ver);
		}
		if (!(p->flags & PKGF_INSTALLED)) {
			free(p->arch);
			p->arch = xstrdup(arch);
		}
	}

	if (!(p->flags & PKGF_ORIGIN))
		suites_merge(&p->repo, slash + 1);

	if ((p->flags & PKGF_INSTALLED) &&
	    strstr(flags, "upgradable from:"))
		p->flags |= PKGF_UPGRADEABLE;

	c->count++;
	if (c->cb && (c->count % 20000) == 0) {
		char msg[96];

		snprintf(msg, sizeof(msg),
			 "Listing available packages... %zu lines",
			 c->count);
		c->cb(c->ud, msg);
	}
}

/* Function: Route apt list output to its parser and retain standard error for diagnostics.
 * Parameters:
 *   ud (void *): Cast to an AptListCtx *, supplied unchanged by proc_run().
 *   line (const char *): NUL-terminated line valid only for the duration of this call.
 *   is_err (int): Nonzero captures standard error instead of parsing standard output.
 * Return (void): No return value.
 */
static void aptlist_cb(void *ud, const char *line, int is_err)
{
	AptListCtx *c = ud;

	if (is_err) {
		buf_puts(&c->err, line);
		buf_putc(&c->err, '\n');
		return;
	}
	aptlist_line(c, line);
}

/* ------------------------------------------------------------------ *
 * Driver                                                               *
 * ------------------------------------------------------------------ */

/* Function: Request a loading-spinner repaint while dpkg-query is silent.
 * Parameters:
 *   ud (void *): Cast to a DpkgCtx *, supplied unchanged by proc_run().
 * Return (int): Always 0, so this callback never requests process cancellation.
 */
static int dpkg_tick(void *ud)
{
	DpkgCtx *c = ud;

	if (c->cb)
		c->cb(c->ud, NULL);
	return 0;
}

/* Function: Request a loading-spinner repaint while apt list is silent.
 * Parameters:
 *   ud (void *): Cast to an AptListCtx *, supplied unchanged by proc_run().
 * Return (int): Always 0, so this callback never requests process cancellation.
 */
static int aptlist_tick(void *ud)
{
	AptListCtx *c = ud;

	if (c->cb)
		c->cb(c->ud, NULL);
	return 0;
}

/* Function: Rebuild a database from installed dpkg records and available apt metadata.
 * Parameters:
 *   db (PkgDB *): Non-NULL initialized database; prior contents are discarded and all existing Package pointers are invalidated.
 *   cb (LoadProgress): Optional progress callback receiving borrowed status text, or NULL for a repaint-only request.
 *   ud (void *): Opaque value passed unchanged to cb.
 * Return (int): 0 on success, or -1 when either child command fails; failure leaves db empty and makes pkgdb_error() describe the command error.
 */
int pkgdb_load(PkgDB *db, LoadProgress cb, void *ud)
{
	DpkgCtx dc;
	AptListCtx ac;
	char *const query_argv[] = {
		"dpkg-query", "-W",
		"-f=${Package}\t${Version}\t${Architecture}\t"
		"${db:Status-Abbrev}\t${Installed-Size}\n",
		NULL
	};
	char *const list_argv[] = { "apt", "list", NULL };
	size_t i;
	int status;

	g_err[0] = '\0';
	pkgdb_clear(db);

	if (cb)
		cb(ud, "Reading installed packages (dpkg-query)...");
	memset(&dc, 0, sizeof(dc));
	dc.db = db;
	dc.cb = cb;
	dc.ud = ud;

	status = proc_run(query_argv, NULL, dpkg_cb, dpkg_tick, &dc);
	if (status != 0) {
		set_error("dpkg-query failed (exit %d)", status);
		pkgdb_clear(db);
		return -1;
	}

	/* The candidate comes from "apt list" below (its version column is
	 * the candidate version - see apt's ListSingleVersion() in
	 * apt-private/private-list.cc).  Origins are resolved lazily per
	 * viewport by pkgdb_fetch_origins(), so startup does not pay for
	 * a full-database "apt-cache policy" round trip (~2.5s). */

	if (cb)
		cb(ud, "Listing available packages (apt list)...");
	memset(&ac, 0, sizeof(ac));
	ac.db = db;
	ac.cb = cb;
	ac.ud = ud;
	buf_init(&ac.err);

	status = proc_run(list_argv, NULL, aptlist_cb, aptlist_tick, &ac);
	if (status != 0) {
		char msg[256];
		char *first = ac.err.data;

		if (first) {
			char *nl = strchr(first, '\n');

			if (nl)
				*nl = '\0';
		}
		fit_into(msg, sizeof(msg), first && *first ? first :
							       "no output");
		set_error("apt list failed (exit %d): %s", status, msg);
		buf_free(&ac.err);
		pkgdb_clear(db);
		return -1;
	}
	buf_free(&ac.err);

	/* Anything dpkg does not report as "ii" is not installed for us;
	 * an upgrade is available when the candidate is newer than the
	 * installed version. */
	for (i = 0; i < db->n; i++) {
		Package *p = &db->v[i];

		if (!(p->flags & PKGF_INSTALLED)) {
			free(p->installed);
			p->installed = NULL;
			p->flags &= (unsigned char)~PKGF_UPGRADEABLE;
			continue;
		}
		if (p->flags & PKGF_UPGRADEABLE)
			continue; /* already flagged by "apt list" */
		if (p->installed && p->candidate &&
		    dpkg_vercmp(p->candidate, p->installed) > 0)
			p->flags |= PKGF_UPGRADEABLE;
	}

	index_build(db);

	if (cb)
		cb(ud, "Package database ready");
	return 0;
}
