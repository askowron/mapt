#ifndef MAPT_VERCMP_H
#define MAPT_VERCMP_H

/* Debian package version comparison, following the algorithm used by
 * libdpkg (dpkg --compare-versions).
 *
 * Returns < 0 when a < b, 0 when a == b and > 0 when a > b. */
int dpkg_vercmp(const char *a, const char *b);

#endif
