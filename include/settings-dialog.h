/* include/settings-dialog.h */
#ifndef __SETTINGS_DIALOG_H__
#define __SETTINGS_DIALOG_H__

#include <gtk/gtk.h>

G_BEGIN_DECLS

typedef struct _AsusdBatteryPlugin AsusdBatteryPlugin;

/* Settings dialog state - FULL DEFINITION */
typedef struct _SettingsDialogState {
    guint dialog_id;
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
} SettingsDialogState;

/* Settings apply context - FULL DEFINITION */
typedef struct _SettingsApplyContext {
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
} SettingsApplyContext;

/* Dialog functions */
void create_settings_dialog(AsusdBatteryPlugin *plugin);
void settings_dialog_sync_from_asusd(AsusdBatteryPlugin *plugin, gboolean keep_dirty);
void settings_dialog_reset_dirty(AsusdBatteryPlugin *plugin);
void settings_dialog_update_ui(AsusdBatteryPlugin *plugin);

/* Apply functions */
void apply_next_setting(SettingsApplyContext *ctx);
void on_settings_apply_complete(SettingsApplyContext *ctx);

/* Callbacks */
void on_close_button_clicked(GtkButton *button, GtkWidget *dialog);
void on_dialog_destroy(GtkWidget *widget, AsusdBatteryPlugin *plugin);
void on_any_setting_changed(GtkWidget *widget, AsusdBatteryPlugin *plugin);
void on_hide_toggle(GtkToggleButton *toggle_button, AsusdBatteryPlugin *plugin);
void on_auto_switch_toggled(GtkToggleButton *toggle_button, GtkWidget *dialog);
void on_one_shot_clicked(GtkButton *button, GtkWidget *dialog);
void on_apply_clicked(GtkButton *button, AsusdBatteryPlugin *plugin);
void on_dialog_property_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_settings_apply_step_done(GObject *source, GAsyncResult *res, gpointer user_data);

G_END_DECLS

#endif /* __SETTINGS_DIALOG_H__ */
