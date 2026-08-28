/*
 * xfce4-asusd-battery - Debug logging system
 */

#include "debug.h"
#include <glib.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

DebugLevel g_debug_level = DEBUG_LEVEL_NONE;
static gboolean debug_warning_shown = FALSE;

void debug_init(void) {
    /* Проверяем, включен ли G_MESSAGES_DEBUG=all, но XFCE4_ASUSD_DEBUG не задан */
    const char *g_messages = g_getenv("G_MESSAGES_DEBUG");
    const char *env = g_getenv("XFCE4_ASUSD_DEBUG");
    gboolean g_messages_all = (g_messages && (g_strcmp0(g_messages, "all") == 0 || 
                                               strstr(g_messages, "all") != NULL));
    
    if (g_messages_all && (!env || g_strcmp0(env, "") == 0 || env[0] == '\0')) {
        if (!debug_warning_shown) {
            g_printerr("\nXFCE4-ASUSD-BATTERY: If you want xfce4-asusd-battery plugin to print debug, also set XFCE4_ASUSD_DEBUG to DEBUG or TRACE\n");
            debug_warning_shown = TRUE;
        }
    }
    
    debug_set_level_from_env();
}

void debug_set_level_from_env(void) {
    const char *env = g_getenv("XFCE4_ASUSD_DEBUG");
    if (!env || g_strcmp0(env, "") == 0 || env[0] == '\0') {
        g_debug_level = DEBUG_LEVEL_NONE;
        return;
    }

    if (g_strcmp0(env, "ERROR") == 0 || g_strcmp0(env, "1") == 0)
        g_debug_level = DEBUG_LEVEL_ERROR;
    else if (g_strcmp0(env, "WARN") == 0 || g_strcmp0(env, "2") == 0)
        g_debug_level = DEBUG_LEVEL_WARN;
    else if (g_strcmp0(env, "INFO") == 0 || g_strcmp0(env, "3") == 0)
        g_debug_level = DEBUG_LEVEL_INFO;
    else if (g_strcmp0(env, "DEBUG") == 0 || g_strcmp0(env, "4") == 0)
        g_debug_level = DEBUG_LEVEL_DEBUG;
    else if (g_strcmp0(env, "TRACE") == 0 || g_strcmp0(env, "5") == 0)
        g_debug_level = DEBUG_LEVEL_TRACE;
    else
        g_debug_level = DEBUG_LEVEL_NONE;
}

void debug_log(DebugLevel level, const char *file, int line, const char *func,
               const char *format, ...) {
    if (g_debug_level < level) return;

    FILE *out = stderr;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t now = tv.tv_sec;
    struct tm *tm_info = localtime(&now);

    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);

    const char *level_names[] = {"NONE", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"};
    const char *level_name = (level <= DEBUG_LEVEL_TRACE) ? level_names[level] : "????";

    const char *color_reset = "\033[0m";
    const char *color_level = "\033[1;31m";
    const char *color_func = "\033[1;36m";

    if (level == DEBUG_LEVEL_WARN) color_level = "\033[1;33m";
    else if (level == DEBUG_LEVEL_INFO) color_level = "\033[1;32m";
    else if (level == DEBUG_LEVEL_DEBUG) color_level = "\033[1;34m";
    else if (level == DEBUG_LEVEL_TRACE) color_level = "\033[1;35m";

    fprintf(out, "[%s.%03ld] %s%5s%s %s%-20s%s:%d - ",
            time_buf, tv.tv_usec / 1000,
            color_level, level_name, color_reset,
            color_func, func, color_reset, line);

    va_list args;
    va_start(args, format);
    vfprintf(out, format, args);
    va_end(args);

    fprintf(out, "\n");
    fflush(out);
}
