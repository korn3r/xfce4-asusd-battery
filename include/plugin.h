/* include/plugin.h */
#ifndef PLUGIN_H
#define PLUGIN_H

#include <gtk/gtk.h>
#include <glib.h>
#include <gio/gio.h>
#include <libxfce4panel/libxfce4panel.h>

/* ========== Forward declarations ========== */
typedef struct _AsusdBatteryPlugin AsusdBatteryPlugin;
typedef struct _AsusdBatteryPluginClass AsusdBatteryPluginClass;
typedef struct _ProfileSettings ProfileSettings;
typedef struct _SettingsDialogState SettingsDialogState;

/* ========== ASUSD состояния ========== */

typedef enum {
    ASUSD_STATE_UNAVAILABLE = 0,
    ASUSD_STATE_CONNECTING = 1,
    ASUSD_STATE_AVAILABLE = 2
} AsusdState;

/* ========== Структура плагина ========== */

struct _AsusdBatteryPlugin {
    GObject parent;
    
    /* Xfce panel */
    XfcePanelPlugin *plugin;
    GtkWidget *button;
    GtkWidget *box;
    GtkWidget *image;
    GtkWidget *label;
    
    /* ASUSD */
    GDBusProxy *asusd_proxy;
    GDBusConnection *connection;
    AsusdState asusd_state;
    gboolean reconnecting;
    guint asusd_retry_timeout_id;
    guint asusd_init_retry_count;
    guint init_load_state;
    
    /* UPower */
    GDBusProxy *upower_proxy;
    gboolean is_on_ac;
    
    /* Profiles */
    GPtrArray *profiles;
    GHashTable *profile_lookup;
    char *current_profile;
    char *last_displayed_profile;
    
    /* Auto switch settings */
    char *auto_switch_ac_profile;
    char *auto_switch_battery_profile;
    gboolean auto_switch_ac_enabled;
    gboolean auto_switch_battery_enabled;
    
    /* Battery limit */
    guint8 current_battery_limit;
    gboolean battery_limit_enabled;
    
    /* Display options */
    gboolean hide_icon;
    gboolean hide_text;
    gboolean hide_notifications;
    
    /* Dialog */
    gboolean settings_dialog_open;
    gboolean saving_settings;
    guint dialog_id_counter;
    SettingsDialogState *dialog_state;
    
    /* Async operations */
    GCancellable *cancellable;
    GQueue *operation_queue;
    gboolean processing_ops;
    guint pending_calls;
    
    /* Notifications */
    time_t last_notification_time;
    
    /* State */
    gboolean is_disposing;
};

struct _AsusdBatteryPluginClass {
    GObjectClass parent_class;
};

/* ========== Type macros ========== */

#define ASUSD_TYPE_BATTERY_PLUGIN (asusd_battery_plugin_get_type())
#define ASUSD_BATTERY_PLUGIN(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), ASUSD_TYPE_BATTERY_PLUGIN, AsusdBatteryPlugin))
#define ASUSD_IS_BATTERY_PLUGIN(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), ASUSD_TYPE_BATTERY_PLUGIN))
#define ASUSD_BATTERY_PLUGIN_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), ASUSD_TYPE_BATTERY_PLUGIN, AsusdBatteryPluginClass))
#define ASUSD_IS_BATTERY_PLUGIN_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), ASUSD_TYPE_BATTERY_PLUGIN))
#define ASUSD_BATTERY_PLUGIN_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS((obj), ASUSD_TYPE_BATTERY_PLUGIN, AsusdBatteryPluginClass))

GType asusd_battery_plugin_get_type(void) G_GNUC_CONST;

/* ========== Public functions ========== */

void update_profile_display(AsusdBatteryPlugin *plugin, gboolean should_notify);
void load_settings(AsusdBatteryPlugin *plugin);
void save_settings(AsusdBatteryPlugin *plugin);

#endif /* PLUGIN_H */
