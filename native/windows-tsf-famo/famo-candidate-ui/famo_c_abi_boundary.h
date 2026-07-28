#pragma once

#if defined(_MSC_VER)
#include <excpt.h>

#define FAMO_C_ABI_SEH_RETURN(expression, fallback)                       \
  __try {                                                                 \
    return (expression);                                                  \
  } __except (EXCEPTION_EXECUTE_HANDLER) {                                \
    return (fallback);                                                    \
  }

#define FAMO_C_ABI_SEH_VOID(expression)                                   \
  __try {                                                                 \
    (expression);                                                         \
    return;                                                               \
  } __except (EXCEPTION_EXECUTE_HANDLER) {                                \
    return;                                                               \
  }
#else
#define FAMO_C_ABI_SEH_RETURN(expression, fallback) return (expression)
#define FAMO_C_ABI_SEH_VOID(expression)                                   \
  do {                                                                    \
    (expression);                                                         \
    return;                                                               \
  } while (false)
#endif
