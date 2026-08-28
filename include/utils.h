/* include/utils.h */
#ifndef __UTILS_H__
#define __UTILS_H__

#include <glib.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include "config.h"
#include "plugin.h"
#include "profile-manager.h"
#include "settings-dialog.h"

#include <libxfce4util/libxfce4util.h>

G_BEGIN_DECLS

/* Async call context */
typedef struct _AsyncCallContext {
    GWeakRef plugin_ref;
    gchar *method_name;
    GVariant *value;
    GAsyncReadyCallback callback;
    gpointer user_data;
    GDestroyNotify destroy_notify;
    gint ref_count;
    guint dialog_id;
    gboolean is_dialog_callback;
} AsyncCallContext;

/* Plugin reference helpers */
AsusdBatteryPlugin* get_plugin_ref(gpointer user_data);
AsusdBatteryPlugin* async_call_context_get_plugin_ref(AsyncCallContext *ctx);

/* Async call context management */
AsyncCallContext* async_call_context_new(AsusdBatteryPlugin *plugin,
                                         const char *method_name,
                                         GVariant *value,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data,
                                         GDestroyNotify destroy_notify);
void async_call_context_free(AsyncCallContext *ctx);
void async_call_context_ref(AsyncCallContext *ctx);
void async_call_context_unref(AsyncCallContext *ctx);

/* Dialog validation */
gboolean is_dialog_valid(AsusdBatteryPlugin *plugin, guint dialog_id);

/* Profile helpers */
const gchar* get_profile_icon(AsusdBatteryPlugin *plugin, const gchar *profile);
const char* asusd_enum_to_default_name(guint32 enum_val);

/* Notification */
gboolean can_send_notification(AsusdBatteryPlugin *plugin);
void send_notification(const gchar *message, const gchar *subtitle, gboolean is_error, const gchar *icon);

/* About dialog */
void create_about_dialog(AsusdBatteryPlugin *plugin);

/* I18n */
void init_i18n(void);

G_END_DECLS

#endif /* __UTILS_H__ */
