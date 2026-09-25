#ifndef MAPT_PKG_H
#define MAPT_PKG_H

#include <stddef.h>

#define PKGF_INSTALLED   1
#define PKGF_UPGRADEABLE 2
#define PKGF_MARKED      4
#define PKGF_ORIGIN      8  /* repo holds the exact origin of the candidate */
#define PKGF_DEPS        16 /* direct dependencies were looked up */
#define PKGF_EXPANDED    32 /* the dependency tree row is expanded */

typedef struct {
	char *name;
	char *installed;  /* installed version, NULL when not installed */
	char *candidate;  /* candidate version, NULL when there is none */
	char *repo;       /* origin of the candidate version, may be NULL */
	char *arch;       /* from dpkg-query, may be NULL */
	unsigned long size_kib; /* Installed-Size, 0 when unknown */
	unsigned char flags;    /* PKGF_* */
	char **deps;      /* direct Depends/PreDepends names, may be NULL */
	size_t n_deps;    /* entries in deps, 0 when none or unknown */
} Package;

typedef struct {
	Package *v;
	size_t n, cap;
	void *index; /* open addressed name hash, may be NULL */
} PkgDB;

/* Progress of the database load.  msg carries the text of the current
 * step; msg == NULL is only a repaint request (the loading spinner is
 * animated by them). */
typedef void (*LoadProgress)(void *ud, const char *msg);

void pkgdb_init(PkgDB *db);
void pkgdb_clear(PkgDB *db);
void pkgdb_free(PkgDB *db);

/* (Re)load the database from "dpkg-query -W", "apt-cache policy" (for the
 * installed packages) and "apt list" (for the whole cache).
 * Returns 0 on success, -1 on error (see pkgdb_error()). */
int pkgdb_load(PkgDB *db, LoadProgress cb, void *ud);

/* Ask "apt-cache policy" about the given packages to replace the rough
 * origin taken from "apt list" with the exact one.  On success every
 * requested package is flagged PKGF_ORIGIN, so it is never asked twice.
 * Returns 0 on success, -1 on failure (the caller stops retrying). */
int pkgdb_fetch_origins(PkgDB *db, char *const names[], size_t n);

/* Ask "apt-cache depends" for the direct dependencies (Depends and
 * PreDepends) of the given packages and store their names in
 * Package.deps.  On success every requested package is flagged
 * PKGF_DEPS, so it is never asked twice (packages with no dependencies
 * keep an empty list).  Returns 0 on success, -1 on failure (the caller
 * stops retrying). */
int pkgdb_fetch_deps(PkgDB *db, char *const names[], size_t n);

const char *pkgdb_error(void);

Package *pkgdb_find(PkgDB *db, const char *name);
Package *pkgdb_at(PkgDB *db, size_t i);

/* Drop every PKGF_MARKED flag. */
void pkgdb_unmark_all(PkgDB *db);

#endif
