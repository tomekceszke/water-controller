#pragma once
/* Minimal test helpers: CHECK*, RUN(test), report(). */
#include <inttypes.h>
#include <stdio.h>

static int s_failures;
static int s_checks;

#define CHECK(cond) do { s_checks++; if (!(cond)) { s_failures++; \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { s_checks++; long long _a = (long long) (a), _b = (long long) (b); if (_a != _b) { \
    s_failures++; printf("  FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); } } while (0)
#define RUN(test) do { printf("%s\n", #test); test(); } while (0)

static int report(void)
{
    printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
