/* src/utils.c */
#include "utils.h"
#include "plugin.h"
#include "debug.h"
#include "config.h"  /* <-- ДОБАВИТЬ ЭТОТ INCLUDE */

#include <libxfce4util/libxfce4util.h>
#include <libnotify/notify.h>
#include <gtk/gtk.h>
#include <time.h>
#include <string.h>

/* ========== Утилиты для уведомлений ========== */

void send_notification(const gchar *summary, const gchar *body, gboolean is_error, const gchar *icon) {
    if (!summary) return;
    
    static gboolean notify_initialized = FALSE;
    if (!notify_initialized) {
        notify_init("xfce4-asusd-battery");
        notify_initialized = TRUE;
    }
    
    NotifyNotification *notification = notify_notification_new(summary, body, icon);
    if (!notification) return;
    
    notify_notification_set_urgency(notification, is_error ? NOTIFY_URGENCY_CRITICAL : NOTIFY_URGENCY_NORMAL);
    notify_notification_set_timeout(notification, is_error ? NOTIFY_EXPIRES_NEVER : NOTIFY_EXPIRES_DEFAULT);
    
    GError *error = NULL;
    if (!notify_notification_show(notification, &error)) {
        if (error) {
            g_error_free(error);
        }
    }
    g_object_unref(notification);
}

gboolean can_send_notification(AsusdBatteryPlugin *plugin) {
    if (!plugin) return FALSE;
    
    /* Если уведомления скрыты — не отправляем */
    if (plugin->hide_notifications) {
        return FALSE;
    }
    
    /* Если anti-flapping выключен — не блокируем уведомления */
    if (!plugin->enable_antiflapping) {
        return TRUE;
    }
    
    /* Если anti-flapping включен — проверяем задержку */
    time_t now = time(NULL);
    if (now - plugin->last_notification_time < 2) return FALSE;
    plugin->last_notification_time = now;
    return TRUE;
}

/* ========== Утилиты для управления плагином ========== */

AsusdBatteryPlugin* get_plugin_ref(gpointer user_data) {
    if (!user_data) return NULL;
    if (G_IS_OBJECT(user_data)) {
        return ASUSD_BATTERY_PLUGIN(g_object_ref(G_OBJECT(user_data)));
    }
    return NULL;
}

void init_i18n(void) {
    static gboolean initialized = FALSE;
    if (initialized) return;
    
    setlocale(LC_ALL, "");
    bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);
    
    initialized = TRUE;
}
