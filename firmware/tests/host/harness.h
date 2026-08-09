#ifndef HARNESS_H
#define HARNESS_H
#include <stdio.h>
#include <string.h>
static int h_checks = 0, h_fails = 0;
#define CHECK(cond) do { h_checks++; if (!(cond)) { h_fails++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ_INT(a, b) do { long long _x = (long long)(a), _y = (long long)(b); h_checks++; \
    if (_x != _y) { h_fails++; \
    fprintf(stderr, "FAIL %s:%d: %s == %lld, want %lld\n", __FILE__, __LINE__, #a, _x, _y); } } while (0)
#define CHECK_EQ_STR(a, b) do { const char *_x = (a), *_y = (b); h_checks++; \
    if (strcmp(_x, _y) != 0) { h_fails++; \
    fprintf(stderr, "FAIL %s:%d: %s == \"%s\", want \"%s\"\n", __FILE__, __LINE__, #a, _x, _y); } } while (0)
#define HARNESS_REPORT() (fprintf(stderr, "%d checks, %d failures\n", h_checks, h_fails), \
    h_fails ? 1 : 0)
#endif
