/* include/utils.h */
#ifndef UTILS_H
#define UTILS_H

#include <glib.h>
#include <gtk/gtk.h>
#include "plugin.h"

/* ========== Forward declaration - AsyncCallContext defined in asusd-client.h ========== */
struct AsyncCallContext;

/* ========== Утилиты для уведомлений ========== */

void send_notification(const gchar *summary, const gchar *body, gboolean is_error, const gchar *icon);
gboolean can_send_notification(AsusdBatteryPlugin *plugin);

/* ========== Утилиты для управления плагином ========== */

AsusdBatteryPlugin* get_plugin_ref(gpointer user_data);
void init_i18n(void);

#endif /* UTILS_H */
