/* include/asusd-client.h */
#ifndef __ASUSD_CLIENT_H__
#define __ASUSD_CLIENT_H__

#include <glib-object.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _AsusdBatteryPlugin AsusdBatteryPlugin;
typedef struct _AsyncCallContext AsyncCallContext;

/* Proxy creation */
void create_asusd_proxy_async(AsusdBatteryPlugin *plugin);
void create_upower_proxy_async(AsusdBatteryPlugin *plugin);

/* D-Bus operations */
void asusd_init_async(AsusdBatteryPlugin *plugin);
void asusd_cleanup(AsusdBatteryPlugin *plugin);
gboolean asusd_retry_init(gpointer user_data);

/* Property operations */
void asusd_get_property_async(AsusdBatteryPlugin *plugin, const char *property,
                              GAsyncReadyCallback callback, gpointer user_data);
void asusd_set_property_async(AsusdBatteryPlugin *plugin, const char *property,
                              GVariant *value, GAsyncReadyCallback callback,
                              gpointer user_data);
void asusd_set_profile_async(AsusdBatteryPlugin *plugin, const gchar *profile_name,
                             GAsyncReadyCallback callback, gpointer user_data);
void asusd_call_async(AsusdBatteryPlugin *plugin, const char *method,
                      GVariant *parameters, GAsyncReadyCallback callback,
                      gpointer user_data);

/* Queue operations */
void asusd_queue_operation(AsusdBatteryPlugin *plugin, const char *method,
                           GVariant *parameters, GAsyncReadyCallback callback,
                           gpointer user_data);
void process_next_operation(AsusdBatteryPlugin *plugin);

/* Callbacks - все должны быть экспортированы */
void on_asusd_proxy_created(GObject *source, GAsyncResult *res, gpointer user_data);
void on_upower_proxy_created(GObject *source, GAsyncResult *res, gpointer user_data);
void on_proxy_properties_changed(GDBusProxy *proxy, GVariant *changed_properties,
                                 GStrv invalidated_properties, gpointer user_data);
void on_property_set_done(GObject *source, GAsyncResult *res, gpointer user_data);
void on_get_property_done(GObject *source, GAsyncResult *res, gpointer user_data);
void on_profile_choices_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_current_profile_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_limit_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_ac_switch_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_ac_profile_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_battery_switch_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_battery_profile_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_one_shot_done(GObject *source, GAsyncResult *res, gpointer user_data);
void on_dialog_property_loaded(GObject *source, GAsyncResult *res, gpointer user_data);

G_END_DECLS

#endif /* __ASUSD_CLIENT_H__ */
