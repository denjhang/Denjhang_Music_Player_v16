#ifndef ERROR_H
#define ERROR_H

#include <stdio.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void logging(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

#ifdef __cplusplus
}
#endif

#endif /* ERROR_H */
