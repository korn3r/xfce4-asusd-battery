/* include/asusd-client.h */
#ifndef ASUSD_CLIENT_H
#define ASUSD_CLIENT_H

#include <glib.h>
#include <gio/gio.h>
#include "plugin.h"

/* ========== AsyncCallContext ========== */

typedef struct AsyncCallContext {
    GWeakRef plugin_ref;
    char *method_name;
    GVariant *value;
    GAsyncReadyCallback callback;
    gpointer user_data;
    GDestroyNotify destroy_notify;
    gint ref_count;
    guint dialog_id;
    gboolean is_dialog_callback;
} AsyncCallContext;

/* ========== AsyncCallContext API ========== */

AsyncCallContext* async_call_context_new(AsusdBatteryPlugin *plugin,
                                         const char *method_name,
                                         GVariant *value,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data,
                                         GDestroyNotify destroy_notify);

void async_call_context_free(AsyncCallContext *ctx);
void async_call_context_ref(AsyncCallContext *ctx);
void async_call_context_unref(AsyncCallContext *ctx);
AsusdBatteryPlugin* async_call_context_get_plugin_ref(AsyncCallContext *ctx);

/* ========== Public API ========== */

void asusd_init_async(AsusdBatteryPlugin *plugin);
gboolean asusd_get_property_async(AsusdBatteryPlugin *plugin, const char *property,
                                  GAsyncReadyCallback callback, gpointer user_data);
void asusd_set_property_async(AsusdBatteryPlugin *plugin, const char *property,
                              GVariant *value, GAsyncReadyCallback callback,
                              gpointer user_data);
void asusd_set_profile_async(AsusdBatteryPlugin *plugin, const gchar *profile_name,
                             GAsyncReadyCallback callback, gpointer user_data);
void asusd_call_async(AsusdBatteryPlugin *plugin, const char *method,
                      GVariant *parameters, GAsyncReadyCallback callback,
                      gpointer user_data);
void asusd_cleanup(AsusdBatteryPlugin *plugin);
gboolean asusd_retry_init(gpointer user_data);

/* ========== Forward declarations for internal functions ========== */

void create_upower_proxy_async(AsusdBatteryPlugin *plugin);
void on_one_shot_done(GObject *source, GAsyncResult *res, gpointer user_data);
void create_about_dialog(AsusdBatteryPlugin *plugin);

#endif /* ASUSD_CLIENT_H */
