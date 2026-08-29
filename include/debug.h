/* include/debug.h */
#ifndef DEBUG_H
#define DEBUG_H

#include <glib.h>
#include <stdio.h>
#include <stdarg.h>

/* ========== Уровни отладки ========== */

typedef enum {
    DEBUG_LEVEL_ERROR = 0,
    DEBUG_LEVEL_WARN = 1,
    DEBUG_LEVEL_INFO = 2,
    DEBUG_LEVEL_DEBUG = 3,
    DEBUG_LEVEL_TRACE = 4
} DebugLevel;

/* ========== Функции отладки ========== */

void debug_init(void);
void debug_set_level(DebugLevel level);
void debug_log(DebugLevel level, const char *file, int line, const char *func, const char *format, ...);

/* ========== Макросы отладки ========== */

#define DEBUG_TRACE(...) debug_log(DEBUG_LEVEL_TRACE, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define DEBUG_DEBUG(...) debug_log(DEBUG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define DEBUG_INFO(...)  debug_log(DEBUG_LEVEL_INFO,  __FILE__, __LINE__, __func__, __VA_ARGS__)
#define DEBUG_WARN(...)  debug_log(DEBUG_LEVEL_WARN,  __FILE__, __LINE__, __func__, __VA_ARGS__)
#define DEBUG_ERROR(...) debug_log(DEBUG_LEVEL_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define DEBUG_TRACE_ENTER() DEBUG_TRACE("→ ENTER")

#endif /* DEBUG_H */
