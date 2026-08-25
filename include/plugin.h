#pragma once

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>
#include <libxfce4util/libxfce4util.h>
#include <xfconf/xfconf.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* Константы */
#define CONFIG_CHANNEL "xfce4-asusd-battery"
#define CONFIG_PROPERTY_PREFIX "/plugins/xfce4-asusd-battery"
#define GETTEXT_PACKAGE "xfce4-asusd-battery"
#define LOCALEDIR "/usr/share/locale"

/* ASUSD D-Bus константы */
#define ASUSD_BUS_NAME "xyz.ljones.Asusd"
#define ASUSD_OBJECT_PATH "/xyz/ljones"
#define ASUSD_INTERFACE "xyz.ljones.Platform"
#define DBUS_PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"
#define ASUSD_TIMEOUT_MS 5000

/* Состояния ASUSD */
typedef enum {
    ASUSD_STATE_UNAVAILABLE,
    ASUSD_STATE_CONNECTING,
    ASUSD_STATE_AVAILABLE
} AsusdState;

/* Структуры */
typedef struct ProfileSettings ProfileSettings;
typedef struct SettingsDialogState SettingsDialogState;
typedef struct AsyncCallContext AsyncCallContext;
typedef struct SettingsApplyContext SettingsApplyContext;

/* Структура плагина как GObject */
#define ASUSD_TYPE_BATTERY_PLUGIN (asusd_battery_plugin_get_type())
G_DECLARE_FINAL_TYPE(AsusdBatteryPlugin, asusd_battery_plugin, ASUSD, BATTERY_PLUGIN, GObject)

struct _AsusdBatteryPlugin {
    GObject parent_instance;
    
    XfcePanelPlugin *plugin;
    GtkWidget *button;
    GtkWidget *box;
    GtkWidget *image;
    GtkWidget *label;
    
    gchar *current_profile;
    GPtrArray *profiles;
    GHashTable *profile_lookup;
    
    gboolean hide_icon;
    gboolean hide_text;
    gboolean hide_notifications;
    
    gboolean battery_limit_enabled;
    gboolean auto_switch_ac_enabled;
    gboolean auto_switch_battery_enabled;
    gchar *auto_switch_ac_profile;
    gchar *auto_switch_battery_profile;
    guint current_battery_limit;
    
    GDBusProxy *asusd_proxy;
    GDBusProxy *upower_proxy;
    GDBusConnection *connection;
    
    gboolean is_on_ac;
    
    AsusdState asusd_state;
    guint asusd_retry_timeout_id;
    guint asusd_init_retry_count;
    
    gchar *last_displayed_profile;
    time_t last_notification_time;
    
    gboolean settings_dialog_open;
    gboolean saving_settings;
    
    SettingsDialogState *dialog_state;
    guint dialog_id_counter;
    
    GQueue *operation_queue;
    gboolean processing_ops;
    guint pending_calls;
    GCancellable *cancellable;
    gint init_load_state;
    
    gboolean is_disposing;
};

/* Структура профиля */
struct ProfileSettings {
    gchar *name;
    gchar *icon;
    gchar *default_name;
    guint32 enum_value;
};

/* Структура состояния диалога настроек */
struct SettingsDialogState {
    GtkWidget *dialog;
    GtkWidget *check_ac;
    GtkWidget *check_battery;
    GtkWidget *combo_ac;
    GtkWidget *combo_battery;
    GtkWidget *limit_check;
    GtkWidget *hide_icon_check;
    GtkWidget *hide_text_check;
    GtkWidget *notifications_check;
    
    gboolean dirty_ac_enabled;
    gboolean dirty_battery_enabled;
    gboolean dirty_ac_profile;
    gboolean dirty_battery_profile;
    gboolean dirty_limit;
    
    gboolean syncing_ui;
    guint dialog_id;
};

/* Контекст для асинхронных операций */
struct AsyncCallContext {
    GWeakRef plugin_ref;
    gchar *method_name;
    GVariant *value;
    GAsyncReadyCallback callback;
    gpointer user_data;
    GDestroyNotify destroy_notify;
    gint ref_count;
    guint dialog_id;
    gboolean is_dialog_callback;
};

/* Контекст для применения настроек */
struct SettingsApplyContext {
    GWeakRef plugin_ref;
    AsusdBatteryPlugin *plugin;
    gboolean new_ac_enabled;
    gboolean new_battery_enabled;
    gchar *new_ac_profile;
    gchar *new_battery_profile;
    gboolean new_limit_enabled;
    guint8 new_limit;
    gint current_step;
    gint total_steps;
    gboolean has_errors;
    gchar **error_messages;
    gint error_count;
    guint apply_id;
};

/* Основные функции */
void asusd_battery_plugin_construct(XfcePanelPlugin *plugin);
void update_profile_display(AsusdBatteryPlugin *plugin, gboolean should_notify);
void load_settings(AsusdBatteryPlugin *plugin);
void save_settings(AsusdBatteryPlugin *plugin);
void init_i18n(void);

/* Callbacks для UI */
void on_button_clicked(GtkWidget *widget, AsusdBatteryPlugin *plugin);
gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, AsusdBatteryPlugin *plugin);
void on_profile_selected(GtkMenuItem *item, AsusdBatteryPlugin *plugin);
void on_set_profile_done(GObject *source, GAsyncResult *res, gpointer user_data);
void on_menu_configure(GtkMenuItem *item, AsusdBatteryPlugin *plugin);
void on_menu_about(GtkMenuItem *item, AsusdBatteryPlugin *plugin);

/* Создание контекста для асинхронных операций */
AsyncCallContext* async_call_context_new(AsusdBatteryPlugin *plugin,
                                         const char *method_name,
                                         GVariant *value,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data,
                                         GDestroyNotify destroy_notify);
void async_call_context_free(AsyncCallContext *ctx);
