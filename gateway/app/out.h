#ifndef OUT_H
#define OUT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Text (JSON) written into a bounded buffer: once something does not fit,
 * full is set and nothing more is written, so a caller checks once at the
 * end instead of after every piece. */
struct out {
    char *buf;
    size_t cap, len;
    bool full;
};

static inline void put(struct out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static inline void put(struct out *o, const char *fmt, ...)
{
    if (o->full)
        return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->cap - o->len)
        o->full = true;
    else
        o->len += (size_t)n;
}

/* The length written, 0 when it did not fit. */
static inline size_t done(const struct out *o) { return o->full ? 0 : o->len; }

#endif
