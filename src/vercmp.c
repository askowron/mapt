/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 APPIT Adam Skowroński
 */
#include "vercmp.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

/*
 * Debian package version comparison.
 *
 * The rules are the ones documented in deb-version(7):
 *
 *   [epoch:]upstream-version[-debian-revision]
 *
 * The string is first split at the LAST hyphen; the upstream part and the
 * revision are then compared independently (the revision is the least
 * significant part, so it is only looked at when the upstream parts are
 * equal).
 *
 * Each part is compared left to right, alternating between a run of
 * non-digit characters (compared "lexically", with letters sorting before
 * non-letters and a tilde sorting before everything, including the end of
 * the part) and a run of digits (compared numerically).
 */

/* Function: order_at
 * Parameters:
 *   s (const char *): Borrowed pointer to at least len readable bytes of a
 *     version component; it is not modified or retained, and NUL termination
 *     is not required.
 *   len (size_t): Component length in bytes in the nonnegative size_t range,
 *     no greater than the accessible input size; zero is valid.
 *   i (size_t): Byte offset in the range 0 through len; i == len denotes the
 *     end sentinel.
 * Return (int): 0 for any digit and for the end sentinel, -1 for '~', the
 *   unsigned byte value for a letter, and that byte value plus 256 for any
 *   other byte.
 */
static int order_at(const char *s, size_t len, size_t i)
{
	unsigned char c = (i < len) ? (unsigned char)s[i] : 0;

	if (isdigit(c))
		return 0;
	if (isalpha(c))
		return c;
	if (c == '~')
		return -1;
	if (c)
		return c + 256;
	return 0; /* end of the part */
}

/* Function: verrevcmp_n
 * Parameters:
 *   a (const char *): Borrowed pointer to at least alen readable bytes; it
 *     is not modified or retained, NUL termination is not required, and a
 *     zero-length input is valid.
 *   alen (size_t): Byte count in the nonnegative size_t range, no greater
 *     than the accessible size of a.
 *   b (const char *): Borrowed pointer to at least blen readable bytes; it
 *     is not modified or retained, NUL termination is not required, and a
 *     zero-length input is valid.
 *   blen (size_t): Byte count in the nonnegative size_t range, no greater
 *     than the accessible size of b.
 * Return (int): -1 when a sorts before b, 0 when they compare equal, and 1
 *   when a sorts after b.
 */
static int verrevcmp_n(const char *a, size_t alen, const char *b, size_t blen)
{
	size_t i = 0, j = 0;

	while (i < alen || j < blen) {
		int firstdiff = 0;

		/* Leading non-digit runs, compared lexically. */
		while ((i < alen && !isdigit((unsigned char)a[i])) ||
		       (j < blen && !isdigit((unsigned char)b[j]))) {
			int ac = order_at(a, alen, i);
			int bc = order_at(b, blen, j);

			if (ac != bc)
				return ac < bc ? -1 : 1;
			i++;
			j++;
		}

		/* Leading digit runs, compared numerically. */
		while (i < alen && a[i] == '0')
			i++;
		while (j < blen && b[j] == '0')
			j++;
		while (i < alen && isdigit((unsigned char)a[i]) &&
		       j < blen && isdigit((unsigned char)b[j])) {
			if (!firstdiff)
				firstdiff = (unsigned char)a[i] -
					    (unsigned char)b[j];
			i++;
			j++;
		}
		/* An empty digit run counts as zero: the longer one wins. */
		if (i < alen && isdigit((unsigned char)a[i]))
			return 1;
		if (j < blen && isdigit((unsigned char)b[j]))
			return -1;
		if (firstdiff)
			return firstdiff < 0 ? -1 : 1;
	}
	return 0;
}

/* Function: scan_epoch
 * Parameters:
 *   v (const char *): Borrowed version buffer with len readable bytes; it is
 *     not modified or retained and need not be NUL terminated.
 *   len (size_t): Buffer length in bytes in the nonnegative size_t range, no
 *     greater than the accessible input size.
 *   epoch (unsigned long *): Valid writable output pointer that receives 0
 *     when no epoch prefix is present; oversized decimal epochs are clamped
 *     to ULONG_MAX.
 * Return (size_t): Byte offset of the component following the epoch's colon
 *   in the range 0 through len, or 0 when no epoch prefix is present.
 */
static size_t scan_epoch(const char *v, size_t len, unsigned long *epoch)
{
	size_t i = 0;
	unsigned long e = 0;

	while (i < len && isdigit((unsigned char)v[i])) {
		unsigned long d = (unsigned long)(v[i] - '0');

		if (e > (~0UL - d) / 10)
			e = ~0UL; /* absurdly large epoch: clamp */
		else
			e = e * 10 + d;
		i++;
	}
	if (i < len && v[i] == ':') {
		*epoch = e;
		return i + 1;
	}
	*epoch = 0;
	return 0;
}

/* Function: split_rev
 * Parameters:
 *   s (const char *): Borrowed buffer containing at least len readable bytes;
 *     it is not modified or retained and need not be NUL terminated.
 *   len (size_t): Buffer length in bytes in the nonnegative size_t range.
 *   up (size_t *): Valid writable output pointer that receives the upstream
 *     component's length in the range 0 through len.
 *   rev (size_t *): Valid writable output pointer that receives the revision's
 *     byte offset in the range 0 through len; it is len when no hyphen exists.
 * Return (void): No return value.  When no hyphen exists, *up is also len.
 */
static void split_rev(const char *s, size_t len, size_t *up, size_t *rev)
{
	size_t i;

	for (i = len; i > 0; i--) {
		if (s[i - 1] == '-') {
			*up = i - 1;
			*rev = i;
			return;
		}
	}
	*up = len;
	*rev = len;
}

/* Function: dpkg_vercmp
 * Parameters:
 *   a (const char *): Borrowed NUL-terminated Debian version string in the
 *     form [epoch:]upstream-version[-debian-revision]; it is not modified or
 *     retained, may be NULL (treated as empty), and has no imposed byte-length
 *     limit beyond the nonnegative size_t range.
 *   b (const char *): Borrowed NUL-terminated Debian version string in the
 *     form [epoch:]upstream-version[-debian-revision]; it is not modified or
 *     retained, may be NULL (treated as empty), and has no imposed byte-length
 *     limit beyond the nonnegative size_t range.
 * Return (int): -1 when a sorts before b, 0 when the versions compare equal,
 *   and 1 when a sorts after b.
 */
int dpkg_vercmp(const char *a, const char *b)
{
	unsigned long ea = 0, eb = 0;
	size_t alen, blen, a0, b0;
	size_t aup, arev, bup, brev;
	int r;

	if (!a)
		a = "";
	if (!b)
		b = "";

	alen = strlen(a);
	blen = strlen(b);

	a0 = scan_epoch(a, alen, &ea);
	b0 = scan_epoch(b, blen, &eb);
	if (ea != eb)
		return ea < eb ? -1 : 1;

	a += a0;
	alen -= a0;
	b += b0;
	blen -= b0;

	split_rev(a, alen, &aup, &arev);
	split_rev(b, blen, &bup, &brev);

	r = verrevcmp_n(a, aup, b, bup);
	if (r)
		return r;
	return verrevcmp_n(a + arev, alen - arev, b + brev, blen - brev);
}
