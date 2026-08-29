/* src/debug.c */
#include "debug.h"
#include <glib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static DebugLevel current_debug_level = DEBUG_LEVEL_INFO;

void debug_init(void) {
    static gboolean initialized = FALSE;
    if (initialized) return;
    
    const char *debug_level = g_getenv("XFCE4_ASUSD_DEBUG");
    if (debug_level) {
        if (g_strcmp0(debug_level, "TRACE") == 0) {
            debug_set_level(DEBUG_LEVEL_TRACE);
        } else if (g_strcmp0(debug_level, "DEBUG") == 0) {
            debug_set_level(DEBUG_LEVEL_DEBUG);
        } else if (g_strcmp0(debug_level, "INFO") == 0) {
            debug_set_level(DEBUG_LEVEL_INFO);
        } else if (g_strcmp0(debug_level, "WARN") == 0) {
            debug_set_level(DEBUG_LEVEL_WARN);
        } else if (g_strcmp0(debug_level, "ERROR") == 0) {
            debug_set_level(DEBUG_LEVEL_ERROR);
        }
    }
    
    initialized = TRUE;
}

void debug_set_level(DebugLevel level) {
    current_debug_level = level;
}

void debug_log(DebugLevel level, const char *file, int line, const char *func, const char *format, ...) {
    if (level > current_debug_level) return;
    
    const char *level_names[] = {"ERROR", "WARN", "INFO", "DEBUG", "TRACE"};
    const char *level_name = level_names[level];
    
    /* Извлекаем только имя файла без пути */
    const char *filename = strrchr(file, '/');
    if (filename) {
        filename++;
    } else {
        filename = file;
    }
    
    va_list args;
    va_start(args, format);
    char *message = g_strdup_vprintf(format, args);
    va_end(args);
    
    g_printerr("[%s] %s:%d - %s: %s\n", level_name, filename, line, func, message);
    g_free(message);
}
