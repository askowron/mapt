#ifndef MAPT_UTIL_H
#define MAPT_UTIL_H

#include <stddef.h>
#include <stdarg.h>

/* Memory helpers: abort the program on allocation failure. */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xvasprintf(const char *fmt, va_list ap);
char *xasprintf(const char *fmt, ...);
void die(const char *fmt, ...);

/* Growable byte buffer, always NUL terminated when non-empty. */
typedef struct {
	char *data;
	size_t len, cap;
} Buf;

void buf_init(Buf *b);
void buf_reset(Buf *b);
void buf_free(Buf *b);
void buf_reserve(Buf *b, size_t extra);
void buf_append(Buf *b, const char *s, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_putc(Buf *b, char c);
void buf_printf(Buf *b, const char *fmt, ...);
char *buf_steal(Buf *b); /* hand over the storage, reset the buffer */

/* NULL terminated argv built from borrowed string pointers. */
typedef struct {
	char **v;
	size_t n, cap;
} Argv;

void argv_init(Argv *a);
void argv_push(Argv *a, char *s);
void argv_free(Argv *a); /* frees the array only, not the strings */

/* String helpers. */
int str_starts(const char *s, const char *pfx);
void str_trim(char *s);
const char *str_after(const char *s, char sep);

/* Copy src into dst (capacity cap including NUL).  When the text does not
 * fit, it is truncated and the last visible character becomes '>'. */
void fit_into(char *dst, size_t cap, const char *src);

/* Render an Installed-Size value (kibibytes) as "1.9 M", "7296 K", ... */
void human_kib(unsigned long kib, char *out, size_t cap);

/* Local time as HH:MM. */
void clock_str(char *out, size_t cap);

/* Milliseconds from a monotonic clock (animation timing). */
long mono_ms(void);

#endif
