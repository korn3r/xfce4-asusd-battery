#ifndef XFCE4_ASUSD_BATTERY_DEBUG_H
#define XFCE4_ASUSD_BATTERY_DEBUG_H

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* Версия плагина */
#ifndef VERSION
#define VERSION "1.0.0"
#endif

/* Уровни отладки */
typedef enum {
    DEBUG_LEVEL_NONE = 0,
    DEBUG_LEVEL_ERROR = 1,
    DEBUG_LEVEL_WARN = 2,
    DEBUG_LEVEL_INFO = 3,
    DEBUG_LEVEL_DEBUG = 4,
    DEBUG_LEVEL_TRACE = 5
} DebugLevel;

/* Глобальный уровень отладки */
extern DebugLevel g_debug_level;

/* Инициализация */
void debug_init(void);
void debug_set_level_from_env(void);

/* Основная функция логирования */
void debug_log(DebugLevel level, const char *file, int line, const char *func,
               const char *format, ...) G_GNUC_PRINTF(5, 6);

/* Макросы */
#define DEBUG_ERROR(...) \
    debug_log(DEBUG_LEVEL_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define DEBUG_WARN(...) \
    debug_log(DEBUG_LEVEL_WARN, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define DEBUG_INFO(...) \
    debug_log(DEBUG_LEVEL_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define DEBUG_DEBUG(...) \
    debug_log(DEBUG_LEVEL_DEBUG, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define DEBUG_TRACE(...) \
    debug_log(DEBUG_LEVEL_TRACE, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define DEBUG_TRACE_ENTER() \
    DEBUG_TRACE("→ ENTER")

#define DEBUG_TRACE_EXIT() \
    DEBUG_TRACE("← EXIT")

#define DEBUG_TRACE_EXIT_RETURN(val) \
    DEBUG_TRACE("← EXIT (return: %s)", #val); \
    return val

#define DEBUG_IS_ENABLED(level) (g_debug_level >= (level))

#endif /* XFCE4_ASUSD_BATTERY_DEBUG_H */
