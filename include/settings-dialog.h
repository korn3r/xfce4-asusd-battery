/* include/settings-dialog.h */
#ifndef SETTINGS_DIALOG_H
#define SETTINGS_DIALOG_H

#include <gtk/gtk.h>
#include "plugin.h"

/* ========== SettingsDialogState структура ========== */

typedef struct _SettingsDialogState {
    GtkWidget *dialog;
    GtkWidget *check_ac;
    GtkWidget *check_battery;
    GtkWidget *combo_ac;
    GtkWidget *combo_battery;
    GtkWidget *limit_check;
    GtkWidget *hide_icon_check;
    GtkWidget *hide_text_check;
    GtkWidget *notifications_check;
    
    /* Anti-flapping */
    GtkWidget *antiflapping_check;
    GtkWidget *custom_time_check;
    GtkWidget *custom_time_entry;
    GtkWidget *custom_time_error_label;
    
    guint dialog_id;
    gboolean syncing_ui;
    
    /* Dirty flags */
    gboolean dirty_ac_enabled;
    gboolean dirty_battery_enabled;
    gboolean dirty_ac_profile;
    gboolean dirty_battery_profile;
    gboolean dirty_limit;
    gboolean dirty_name;
    gboolean dirty_icon;
    gboolean dirty_antiflapping;
    gboolean dirty_custom_time;
    gboolean dirty_timeout;
} SettingsDialogState;

/* ========== Settings context for apply ========== */

typedef struct _SettingsApplyContext {
    GWeakRef plugin_ref;
    AsusdBatteryPlugin *plugin;
    gboolean new_ac_enabled;
    gboolean new_battery_enabled;
    char *new_ac_profile;
    char *new_battery_profile;
    gboolean new_limit_enabled;
    guint8 new_limit;
    int current_step;
    int total_steps;
    gboolean has_errors;
    char **error_messages;
    int error_count;
} SettingsApplyContext;

/* ========== Public functions ========== */

void create_settings_dialog(AsusdBatteryPlugin *plugin);
void settings_dialog_sync_from_asusd(AsusdBatteryPlugin *plugin, gboolean keep_dirty);
void settings_dialog_update_ui(AsusdBatteryPlugin *plugin);
void settings_dialog_reset_dirty(AsusdBatteryPlugin *plugin);

/* ========== Callbacks ========== */

void on_apply_clicked(GtkButton *button, AsusdBatteryPlugin *plugin);
void on_close_button_clicked(GtkButton *button, GtkWidget *dialog);
void on_dialog_destroy(GtkWidget *widget, AsusdBatteryPlugin *plugin);
void on_any_setting_changed(GtkWidget *widget, AsusdBatteryPlugin *plugin);
void on_hide_toggle(GtkToggleButton *toggle_button, AsusdBatteryPlugin *plugin);
void on_auto_switch_toggled(GtkToggleButton *toggle_button, GtkWidget *dialog);
void on_one_shot_clicked(GtkButton *button, GtkWidget *dialog);
void on_dialog_property_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_antiflapping_toggled(GtkToggleButton *toggle_button, AsusdBatteryPlugin *plugin);
void on_custom_time_toggled(GtkToggleButton *toggle_button, AsusdBatteryPlugin *plugin);

/* ========== Internal functions ========== */

gboolean is_dialog_valid(AsusdBatteryPlugin *plugin, guint dialog_id);
void apply_next_setting(SettingsApplyContext *ctx);
void on_settings_apply_step_done(GObject *source, GAsyncResult *res, gpointer user_data);
void on_settings_apply_complete(SettingsApplyContext *ctx);

#endif /* SETTINGS_DIALOG_H */
