#pragma once

#include <stdio.h>

#if NDEBUG

#define DEBUG_PRINT(...)

#else

#define DEBUG_PRINT(...)                                                       \
  do {                                                                         \
    fprintf(stderr, __VA_ARGS__);                                              \
    fflush(stderr);                                                            \
  } while (0)

#endif
