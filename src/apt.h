#ifndef MAPT_APT_H
#define MAPT_APT_H

#include <stddef.h>
#include "pkg.h"

/* Every function returns the exit status of the command, or a negative
 * value when the action was cancelled (or sudo is unavailable). */

int apt_update(void);
int apt_install(const char *title, char *const *names, size_t n);
int apt_reinstall(const char *title, char *const *names, size_t n);
int apt_remove(const char *title, char *const *names, size_t n, int purge);
int apt_upgrade_all(void);
int apt_update_upgrade(void);
int apt_dist_upgrade(void);
int apt_autoremove(void);

/* Formatted, ready to be shown in dlg_text().  Caller frees. */
char *apt_pkg_info(const Package *p);
char *apt_pkg_files(const Package *p);

#endif
