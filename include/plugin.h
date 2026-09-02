#ifndef PLUGIN_H
#define PLUGIN_H

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

G_BEGIN_DECLS

/* ========== ASUSD состояния ========== */
#define ASUSD_STATE_UNAVAILABLE 0
#define ASUSD_STATE_CONNECTING  1
#define ASUSD_STATE_AVAILABLE   2

/* Anti-flapping */
#define DEFAULT_TIMEOUT_MS 1500

/* ========== GObject определения ========== */

#define ASUSD_TYPE_BATTERY_PLUGIN (asusd_battery_plugin_get_type())
G_DECLARE_FINAL_TYPE(AsusdBatteryPlugin, asusd_battery_plugin, ASUSD, BATTERY_PLUGIN, XfcePanelPlugin)

struct _AsusdBatteryPlugin {
    XfcePanelPlugin __parent__;
    
    /* Plugin reference */
    XfcePanelPlugin *plugin;
    
    /* UI Elements */
    GtkWidget *button;
    GtkWidget *image;
    GtkWidget *label;
    GtkWidget *box;
    
    /* D-Bus */
    GDBusProxy *asusd_proxy;
    GDBusProxy *upower_proxy;
    GDBusConnection *connection;
    GCancellable *cancellable;
    
    /* Profile data */
    GPtrArray *profiles;
    GHashTable *profile_lookup;
    gchar *current_profile;
    gchar *last_displayed_profile;
    gchar *auto_switch_ac_profile;
    gchar *auto_switch_battery_profile;
    
    /* Settings */
    gboolean hide_icon;
    gboolean hide_text;
    gboolean hide_notifications;
    gboolean enable_antiflapping;
    gboolean custom_time_enabled;
    guint custom_timeout_ms;
    gboolean no_battery;
    gboolean battery_limit_enabled;
    guint8 current_battery_limit;
    
    /* ASUSD state */
    gint asusd_state;
    gint asusd_init_retry_count;
    gint init_load_state;
    gboolean reconnecting;
    guint asusd_retry_timeout_id;
    
    /* UPower state */
    gboolean is_on_ac;
    gboolean auto_switch_ac_enabled;
    gboolean auto_switch_battery_enabled;
    
    /* Settings dialog */
    struct _SettingsDialogState *dialog_state;
    gboolean settings_dialog_open;
    gboolean saving_settings;
    guint dialog_id_counter;
    
    /* Notification debounce */
    time_t last_notification_time;
    guint notification_timeout_id;
    gchar *pending_notification_profile;
    gchar *last_notified_profile;
    
    /* Operations queue */
    GQueue *operation_queue;
    gboolean processing_ops;
    guint pending_calls;
    
    /* Disposing flag */
    gboolean is_disposing;
};

/* ========== Public functions ========== */

/* Основные функции плагина */
void update_profile_display(AsusdBatteryPlugin *plugin, gboolean should_notify);
void load_settings(AsusdBatteryPlugin *plugin);
void save_settings(AsusdBatteryPlugin *plugin);

/* UI Callbacks */
void on_button_clicked(GtkWidget *widget, AsusdBatteryPlugin *plugin);
void on_profile_selected(GtkMenuItem *item, AsusdBatteryPlugin *plugin);
void on_set_profile_done(GObject *source, GAsyncResult *res, gpointer user_data);
void on_menu_configure(GtkMenuItem *item, AsusdBatteryPlugin *plugin);
void on_menu_about(GtkMenuItem *item, AsusdBatteryPlugin *plugin);

/* Notification functions */
void schedule_notification(AsusdBatteryPlugin *plugin, const gchar *profile);

/* About dialog */
void create_about_dialog(AsusdBatteryPlugin *plugin);
void create_settings_dialog(AsusdBatteryPlugin *plugin);

/* Утилиты */
AsusdBatteryPlugin* get_plugin_ref(gpointer user_data);

G_END_DECLS

#endif /* PLUGIN_H */
