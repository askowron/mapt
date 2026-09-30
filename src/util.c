/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 APPIT Adam Skowroński
 */
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Function: die
 * Parameters:
 *   fmt (const char *): Borrowed, non-NULL printf-style format string.  Its
 *     variadic arguments are borrowed values whose types, meanings, and valid
 *     ranges are defined by fmt.
 * Return (void): Does not return; writes the formatted diagnostic and a
 *   newline to stderr, then terminates the process with exit status 1.
 */
void die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(1);
}

/* Function: xmalloc
 * Parameters:
 *   n (size_t): Requested allocation size in bytes in the nonnegative size_t
 *     range; zero is treated as one byte.
 * Return (void *): A new uninitialized allocation whose ownership passes to
 *   the caller and which must eventually be released with free().  Terminates
 *   the process if allocation fails.
 */
void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);

	if (!p)
		die("mapt: out of memory");
	return p;
}

/* Function: xcalloc
 * Parameters:
 *   n (size_t): Element count in the nonnegative size_t range; zero is treated
 *     as one element.
 *   sz (size_t): Byte count per element in the nonnegative size_t range; zero
 *     is treated as one byte.
 * Return (void *): A new zero-initialized allocation owned by the caller and
 *   releaseable with free().  Terminates the process if allocation fails.
 */
void *xcalloc(size_t n, size_t sz)
{
	void *p = calloc(n ? n : 1, sz ? sz : 1);

	if (!p)
		die("mapt: out of memory");
	return p;
}

/* Function: xrealloc
 * Parameters:
 *   p (void *): Existing allocation borrowed for the call; NULL is valid and
 *     makes this function behave like xmalloc().  On success, the old
 *     allocation is invalid.
 *   n (size_t): New allocation size in bytes in the nonnegative size_t range;
 *     zero is treated as one byte.
 * Return (void *): Resized storage owned by the caller, with contents not
 *   initialized by this function.  Terminates the process if resizing fails.
 */
void *xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);

	if (!q)
		die("mapt: out of memory");
	return q;
}

/* Function: xstrdup
 * Parameters:
 *   s (const char *): Borrowed NUL-terminated string, or NULL for the empty
 *     string; it is not modified or retained.
 * Return (char *): A newly allocated, NUL-terminated copy owned by the caller
 *   and releaseable with free().  Terminates the process if allocation fails.
 */
char *xstrdup(const char *s)
{
	char *p = strdup(s ? s : "");

	if (!p)
		die("mapt: out of memory");
	return p;
}

/* Function: xstrndup
 * Parameters:
 *   s (const char *): Borrowed NUL-terminated string, or NULL for the empty
 *     string; it is not modified or retained.
 *   n (size_t): Maximum number of bytes to copy in the nonnegative size_t
 *     range.
 * Return (char *): A newly allocated, always NUL-terminated copy owned by the
 *   caller and releaseable with free().  Terminates the process if allocation
 *   fails.
 */
char *xstrndup(const char *s, size_t n)
{
	size_t l = s ? strlen(s) : 0;
	char *p;

	if (l > n)
		l = n;
	p = xmalloc(l + 1);
	if (l)
		memcpy(p, s, l);
	p[l] = '\0';
	return p;
}

/* Function: xvasprintf
 * Parameters:
 *   fmt (const char *): Borrowed, non-NULL, NUL-terminated printf-style format
 *     string with no numeric unit or fixed range; types, meanings, and valid
 *     ranges of formatted arguments are defined by its conversion specifications.
 *   ap (va_list): Borrowed va_list positioned at its first argument; the caller
 *     retains it and remains responsible for va_end() when required by origin.
 * Return (char *): A newly allocated, NUL-terminated string owned by the caller
 *   and releaseable with free().  Terminates the process if allocation fails.
 */
char *xvasprintf(const char *fmt, va_list ap)
{
	va_list a2;
	int n;
	char *p;

	va_copy(a2, ap);
	n = vsnprintf(NULL, 0, fmt, a2);
	va_end(a2);
	if (n < 0)
		n = 0;
	p = xmalloc((size_t)n + 1);
	vsnprintf(p, (size_t)n + 1, fmt, ap);
	return p;
}

/* Function: xasprintf
 * Parameters:
 *   fmt (const char *): Borrowed, non-NULL printf-style format string.  Its
 *     variadic arguments are borrowed values whose types, meanings, and valid
 *     ranges are defined by fmt.
 * Return (char *): A newly allocated, NUL-terminated formatted string whose
 *   ownership passes to the caller and which must be released with free().
 *   Terminates the process if allocation fails.
 */
char *xasprintf(const char *fmt, ...)
{
	va_list ap;
	char *s;

	va_start(ap, fmt);
	s = xvasprintf(fmt, ap);
	va_end(ap);
	return s;
}

/* Function: buf_init
 * Parameters:
 *   b (Buf *): Valid, writable Buf object with no numeric range; its storage
 *     remains caller-owned, and any storage it already owns must be freed
 *     before this reset.
 * Return (void): No return value; b is initialized as empty.
 */
void buf_init(Buf *b)
{
	b->data = NULL;
	b->len = b->cap = 0;
}

/* Function: buf_reset
 * Parameters:
 *   b (Buf *): Valid, writable, initialized Buf whose storage remains owned by
 *     the caller.
 * Return (void): No return value.  The length becomes zero bytes; any data
 *   allocation is reused and, when present, remains NUL-terminated.
 */
void buf_reset(Buf *b)
{
	b->len = 0;
	if (b->data)
		b->data[0] = '\0';
}

/* Function: buf_free
 * Parameters:
 *   b (Buf *): Valid, writable, initialized, caller-owned Buf that may be
 *     reused or reinitialized afterward.
 * Return (void): No return value.  Releases all storage owned by b and leaves
 *   it initialized and empty.
 */
void buf_free(Buf *b)
{
	free(b->data);
	buf_init(b);
}

/* Function: buf_reserve
 * Parameters:
 *   b (Buf *): Valid, writable, initialized, caller-owned Buf.
 *   extra (size_t): Additional payload capacity in bytes in the nonnegative
 *     size_t range; b->len + extra + 1 must be representable.
 * Return (void): No return value.  Ensures capacity for the current contents,
 *   extra more bytes, and a trailing NUL without changing the current length;
 *   newly allocated bytes are not initialized.
 */
void buf_reserve(Buf *b, size_t extra)
{
	size_t need = b->len + extra + 1;

	if (need <= b->cap)
		return;
	if (b->cap == 0)
		b->cap = 256;
	while (b->cap < need)
		b->cap *= 2;
	b->data = xrealloc(b->data, b->cap);
}

/* Function: buf_append
 * Parameters:
 *   b (Buf *): Valid, writable, initialized, caller-owned Buf.
 *   s (const char *): Borrowed readable array of n bytes; it is not modified
 *     or retained, may be NULL only when n is zero, and must not overlap b's
 *     storage.
 *   n (size_t): Byte count in the nonnegative size_t range; the final length
 *     plus one NUL must be representable.
 * Return (void): No return value.  Appends the raw bytes, preserves embedded
 *   NUL bytes, and maintains a trailing NUL terminator.
 */
void buf_append(Buf *b, const char *s, size_t n)
{
	if (!n)
		return;
	buf_reserve(b, n);
	memcpy(b->data + b->len, s, n);
	b->len += n;
	b->data[b->len] = '\0';
}

/* Function: buf_puts
 * Parameters:
 *   b (Buf *): Valid, writable, initialized, caller-owned Buf.
 *   s (const char *): Borrowed NUL-terminated string, or NULL; it is not
 *     modified or retained, and its byte length must fit available storage.
 * Return (void): No return value.  Appends s excluding its terminating NUL.
 */
void buf_puts(Buf *b, const char *s)
{
	if (s)
		buf_append(b, s, strlen(s));
}

/* Function: buf_putc
 * Parameters:
 *   b (Buf *): Valid, writable, initialized, caller-owned Buf.
 *   c (char): Single byte to append; any value representable by char is valid.
 * Return (void): No return value.
 */
void buf_putc(Buf *b, char c)
{
	buf_reserve(b, 1);
	b->data[b->len++] = c;
	b->data[b->len] = '\0';
}

/* Function: buf_printf
 * Parameters:
 *   b (Buf *): Valid, writable, initialized, caller-owned Buf.
 *   fmt (const char *): Borrowed, non-NULL printf-style format string.  Its
 *     variadic arguments are borrowed values whose types, meanings, and valid
 *     ranges are defined by fmt.
 * Return (void): No return value.  Appends formatted text; an empty or invalid
 *   formatting result leaves b unchanged.
 */
void buf_printf(Buf *b, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	buf_reserve(b, (size_t)n);
	va_start(ap, fmt);
	vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
	va_end(ap);
	b->len += (size_t)n;
}

/* Function: buf_steal
 * Parameters:
 *   b (Buf *): Valid, writable, initialized, caller-owned Buf.
 * Return (char *): The buffer's NUL-terminated allocation, or a fresh empty
 *   string when b had no allocation.  Ownership passes to the caller, which
 *   must release it with free(); b is reinitialized as empty.
 */
char *buf_steal(Buf *b)
{
	char *p;

	if (!b->data)
		return xstrdup("");
	buf_reserve(b, 0); /* makes sure the buffer is NUL terminated */
	p = b->data;
	buf_init(b);
	return p;
}

/* Function: argv_init
 * Parameters:
 *   a (Argv *): Valid, writable, caller-owned Argv object; any vector array it
 *     already owns must be freed before this reset.
 * Return (void): No return value; a is initialized as an empty, NULL-terminated
 *   argument vector.
 */
void argv_init(Argv *a)
{
	a->v = NULL;
	a->n = a->cap = 0;
}

/* Function: argv_push
 * Parameters:
 *   a (Argv *): Valid, writable, initialized, caller-owned Argv.
 *   s (char *): Borrowed, unitless pointer stored without copying or ownership
 *     transfer; it may be NULL, although an entry used for execution must
 *     point to a NUL-terminated string.  The caller keeps it alive for the
 *     vector's use and remains responsible for freeing it.
 * Return (void): No return value.  Appends s and keeps a->v NULL terminated.
 */
void argv_push(Argv *a, char *s)
{
	if (a->n + 2 > a->cap) {
		a->cap = a->cap ? a->cap * 2 : 8;
		a->v = xrealloc(a->v, a->cap * sizeof(*a->v));
	}
	a->v[a->n++] = s;
	a->v[a->n] = NULL;
}

/* Function: argv_free
 * Parameters:
 *   a (Argv *): Valid, writable, initialized, caller-owned Argv that may be
 *     reused afterward.
 * Return (void): No return value.  Frees only the pointer array owned by a,
 *   leaves all referenced strings untouched, and reinitializes a as empty.
 */
void argv_free(Argv *a)
{
	free(a->v);
	argv_init(a);
}

/* Function: str_starts
 * Parameters:
 *   s (const char *): Borrowed NUL-terminated string, or NULL; it is not
 *     modified or retained and otherwise has an unrestricted byte length.
 *   pfx (const char *): Borrowed NUL-terminated prefix, or NULL; it is not
 *     modified or retained and otherwise has an unrestricted byte length.
 * Return (int): 1 only when both strings are present and pfx matches the
 *   beginning of s exactly; 0 in all other cases.
 */
int str_starts(const char *s, const char *pfx)
{
	return s && pfx && strncmp(s, pfx, strlen(pfx)) == 0;
}

/* Function: str_trim
 * Parameters:
 *   s (char *): Borrowed, mutable NUL-terminated string, or NULL; it is neither
 *     retained nor reallocated, and trimming operates in bytes with no other
 *     numeric range.
 * Return (void): No return value.  Removes leading and trailing spaces, tabs,
 *   carriage returns, and newlines in place; NULL does nothing.
 */
void str_trim(char *s)
{
	char *p;
	size_t n;

	if (!s)
		return;
	p = s;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);
	n = strlen(s);
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
		     s[n - 1] == '\r' || s[n - 1] == '\n'))
		s[--n] = '\0';
}

/* Function: str_after
 * Parameters:
 *   s (const char *): Borrowed NUL-terminated string with unrestricted byte
 *     length, or NULL; it is not modified or retained.
 *   sep (char): Separator byte; any char value is valid.
 * Return (const char *): A borrowed pointer immediately after the first sep,
 *   still owned and bounded by s, or NULL when s is NULL or contains no sep.
 *   The result must not outlive s.
 */
const char *str_after(const char *s, char sep)
{
	const char *p = s ? strchr(s, sep) : NULL;

	return p ? p + 1 : NULL;
}

/* Function: fit_into
 * Parameters:
 *   dst (char *): Writable output array of cap bytes, or NULL only when cap is
 *     zero.
 *   cap (size_t): Output capacity in bytes, including the terminating NUL, in
 *     the nonnegative size_t range.
 *   src (const char *): Borrowed unrestricted NUL-terminated string, or NULL
 *     for empty output; it is not modified or retained and must not overlap dst.
 * Return (void): No return value.  Copies src, truncating when necessary so the
 *   final visible character is '>'.
 */
void fit_into(char *dst, size_t cap, const char *src)
{
	size_t l;

	if (!cap)
		return;
	if (!src) {
		dst[0] = '\0';
		return;
	}
	l = strlen(src);
	if (l < cap) {
		memcpy(dst, src, l + 1);
		return;
	}
	if (cap < 2) {
		dst[0] = '\0';
		return;
	}
	memcpy(dst, src, cap - 2);
	dst[cap - 2] = '>';
	dst[cap - 1] = '\0';
}

/* Function: human_kib
 * Parameters:
 *   kib (unsigned long): Non-negative size in kibibytes (1024 bytes) spanning
 *     the full unsigned long range.
 *   out (char *): Caller-owned writable storage with cap bytes including the NUL
 *     terminator; it may be NULL only when cap is zero.
 *   cap (size_t): Output capacity in bytes in the nonnegative size_t range.
 * Return (void): No return value.  Writes a human-readable byte value using
 *   binary scaling; output may be truncated according to snprintf().
 */
void human_kib(unsigned long kib, char *out, size_t cap)
{
	static const char *unit[] = { "B", "K", "M", "G", "T" };
	double v = (double)kib * 1024.0;
	int u = 0;

	if (kib == 0) {
		snprintf(out, cap, "0");
		return;
	}
	while (v >= 1024.0 && u < 4) {
		v /= 1024.0;
		u++;
	}
	if (u == 0)
		snprintf(out, cap, "%.0f %s", v, unit[u]);
	else if (v < 10.0)
		snprintf(out, cap, "%.1f %s", v, unit[u]);
	else
		snprintf(out, cap, "%.0f %s", v, unit[u]);
}

/* Function: clock_str
 * Parameters:
 *   out (char *): Caller-owned writable storage with cap bytes including the NUL
 *     terminator.
 *   cap (size_t): Output capacity in bytes in the nonnegative size_t range; a
 *     value of at least 6 permits the fixed-width result to fit.
 * Return (void): No return value; writes the current local wall-clock time as
 *   HH:MM.
 */
void clock_str(char *out, size_t cap)
{
	time_t t = time(NULL);
	struct tm tmv;

	localtime_r(&t, &tmv);
	strftime(out, cap, "%H:%M", &tmv);
}

/* Function: mono_ms
 * Parameters: None.
 * Return (long): Milliseconds elapsed on the process-wide monotonic clock,
 *   whose origin is unspecified and which is nondecreasing while the converted
 *   value is representable.  Returns 0 if clock_gettime() fails; zero may also
 *   be a successful clock-origin reading, so the result is a duration rather
 *   than calendar time.
 */
long mono_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}
