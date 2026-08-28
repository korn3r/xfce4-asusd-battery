/* include/plugin.h */
#ifndef __PLUGIN_H__
#define __PLUGIN_H__

#include <glib-object.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <libxfce4panel/libxfce4panel.h>

G_BEGIN_DECLS

#define ASUSD_TYPE_BATTERY_PLUGIN (asusd_battery_plugin_get_type())
#define ASUSD_BATTERY_PLUGIN(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), ASUSD_TYPE_BATTERY_PLUGIN, AsusdBatteryPlugin))
#define ASUSD_IS_BATTERY_PLUGIN(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), ASUSD_TYPE_BATTERY_PLUGIN))
#define ASUSD_BATTERY_PLUGIN_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), ASUSD_TYPE_BATTERY_PLUGIN, AsusdBatteryPluginClass))

/* ASUSD states */
typedef enum {
    ASUSD_STATE_UNAVAILABLE = 0,
    ASUSD_STATE_CONNECTING = 1,
    ASUSD_STATE_AVAILABLE = 2
} AsusdState;

/* Forward declarations */
typedef struct _AsusdBatteryPlugin AsusdBatteryPlugin;
typedef struct _AsusdBatteryPluginClass AsusdBatteryPluginClass;
typedef struct _SettingsDialogState SettingsDialogState;
typedef struct _ProfileSettings ProfileSettings;

/* Main plugin structure */
struct _AsusdBatteryPlugin {
    GObject parent_instance;
    
    /* UI */
    XfcePanelPlugin *plugin;
    GtkWidget *button;
    GtkWidget *box;
    GtkWidget *image;
    GtkWidget *label;
    
    /* D-Bus */
    GDBusProxy *asusd_proxy;
    GDBusProxy *upower_proxy;
    GDBusConnection *connection;
    GCancellable *cancellable;
    guint asusd_retry_timeout_id;
    guint asusd_init_retry_count;
    
    /* Reconnection guard - только флаг, reconnect_source_id удален */
    gboolean reconnecting;      /* Flag to prevent duplicate reconnection */
    
    /* State */
    AsusdState asusd_state;
    gchar *current_profile;
    gchar *last_displayed_profile;
    gboolean is_on_ac;
    guint init_load_state;
    gboolean is_disposing;
    gint pending_calls;
    gboolean processing_ops;
    gboolean saving_settings;
    gboolean settings_dialog_open;
    guint dialog_id_counter;
    
    /* Settings */
    gboolean hide_icon;
    gboolean hide_text;
    gboolean hide_notifications;
    gboolean auto_switch_ac_enabled;
    gboolean auto_switch_battery_enabled;
    gchar *auto_switch_ac_profile;
    gchar *auto_switch_battery_profile;
    guint8 current_battery_limit;
    gboolean battery_limit_enabled;
    
    /* Profiles */
    GPtrArray *profiles;
    GHashTable *profile_lookup;
    
    /* Dialog state */
    SettingsDialogState *dialog_state;
    
    /* Queue */
    GQueue *operation_queue;
    
    /* Notification throttling */
    time_t last_notification_time;
};

struct _AsusdBatteryPluginClass {
    GObjectClass parent_class;
};

GType asusd_battery_plugin_get_type(void);

/* Main functions */
void asusd_battery_plugin_construct(XfcePanelPlugin *plugin);
void update_profile_display(AsusdBatteryPlugin *plugin, gboolean should_notify);
void load_settings(AsusdBatteryPlugin *plugin);
void save_settings(AsusdBatteryPlugin *plugin);

/* UI Callbacks */
void on_button_clicked(GtkWidget *widget, AsusdBatteryPlugin *plugin);
gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, AsusdBatteryPlugin *plugin);
void on_profile_selected(GtkMenuItem *item, AsusdBatteryPlugin *plugin);
void on_set_profile_done(GObject *source, GAsyncResult *res, gpointer user_data);
void on_menu_configure(GtkMenuItem *item, AsusdBatteryPlugin *plugin);
void on_menu_about(GtkMenuItem *item, AsusdBatteryPlugin *plugin);

G_END_DECLS

#endif /* __PLUGIN_H__ */
