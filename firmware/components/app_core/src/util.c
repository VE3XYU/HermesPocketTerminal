#include "util.h"
#include <string.h>

size_t str_copy(char *dst, size_t cap, const char *src) {
    if (cap == 0) return 0;
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
    return n;
}
