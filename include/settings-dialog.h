#pragma once

#include "plugin.h"

/* Диалог настроек */
void create_settings_dialog(AsusdBatteryPlugin *plugin);
void settings_dialog_sync_from_asusd(AsusdBatteryPlugin *plugin, gboolean keep_dirty);
void settings_dialog_update_ui(AsusdBatteryPlugin *plugin);
void settings_dialog_reset_dirty(AsusdBatteryPlugin *plugin);

/* Callbacks для диалога */
void on_dialog_property_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_close_button_clicked(GtkButton *button, GtkWidget *dialog);
void on_apply_clicked(GtkButton *button, AsusdBatteryPlugin *plugin);
void on_any_setting_changed(GtkWidget *widget, AsusdBatteryPlugin *plugin);
void on_hide_toggle(GtkToggleButton *toggle_button, AsusdBatteryPlugin *plugin);
void on_auto_switch_toggled(GtkToggleButton *toggle_button, GtkWidget *dialog);
void on_dialog_destroy(GtkWidget *widget, AsusdBatteryPlugin *plugin);
void on_one_shot_clicked(GtkButton *button, GtkWidget *dialog);

/* Применение настроек */
void apply_next_setting(SettingsApplyContext *ctx);
void on_settings_apply_step_done(GObject *source, GAsyncResult *res, gpointer user_data);
void on_settings_apply_complete(SettingsApplyContext *ctx);
