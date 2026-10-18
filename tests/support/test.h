#pragma once

#include <cmath>
#include <cstdio>

static int g_failures = 0;

#define CHECK(cond)                                          \
  do {                                                       \
    if (!(cond)) {                                           \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                          \
    }                                                        \
  } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))
#define CHECK_NEAR(a, b, tolerance) CHECK(std::abs((a) - (b)) <= (tolerance))

inline int test_result() {
  if (g_failures)
    printf("%d FAILURES\n", g_failures);
  else
    puts("all tests passed");
  return g_failures ? 1 : 0;
}
