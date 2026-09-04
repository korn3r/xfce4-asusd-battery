/* src/settings-dialog.c */
#include "settings-dialog.h"
#include "utils.h"
#include "profile-manager.h"
#include "config.h"
#include "plugin.h"
#include "asusd-client.h"
#include "debug.h"
#include <libxfce4util/libxfce4util.h>
#include <stdlib.h>

/* ========== Forward declarations ========== */
void on_one_shot_done(GObject *source, GAsyncResult *res, gpointer user_data);
void apply_next_setting(SettingsApplyContext *ctx);
void on_settings_apply_step_done(GObject *source, GAsyncResult *res, gpointer user_data);
void on_settings_apply_complete(SettingsApplyContext *ctx);
gboolean is_dialog_valid(AsusdBatteryPlugin *plugin, guint dialog_id);
static gboolean validate_custom_time(GtkEntry *entry, GtkLabel *error_label, guint *out_value);
void on_custom_time_changed(GtkWidget *widget, AsusdBatteryPlugin *plugin);
void on_fixed_width_toggled(GtkToggleButton *toggle_button, AsusdBatteryPlugin *plugin);
void on_notifications_toggled(GtkToggleButton *toggle_button, AsusdBatteryPlugin *plugin);

/* ========== Реализация is_dialog_valid ========== */
gboolean is_dialog_valid(AsusdBatteryPlugin *plugin, guint dialog_id) {
    if (!plugin) return FALSE;
    if (!plugin->dialog_state) return FALSE;
    if (plugin->dialog_state->dialog_id != dialog_id) return FALSE;
    if (!plugin->settings_dialog_open) return FALSE;
    if (plugin->is_disposing) return FALSE;
    return TRUE;
}

/* ========== Функции диалога настроек ========== */

void settings_dialog_reset_dirty(AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->dialog_state) return;
    plugin->dialog_state->dirty_ac_enabled = FALSE;
    plugin->dialog_state->dirty_battery_enabled = FALSE;
    plugin->dialog_state->dirty_ac_profile = FALSE;
    plugin->dialog_state->dirty_battery_profile = FALSE;
    plugin->dialog_state->dirty_limit = FALSE;
    plugin->dialog_state->dirty_name = FALSE;
    plugin->dialog_state->dirty_icon = FALSE;
    plugin->dialog_state->dirty_antiflapping = FALSE;
    plugin->dialog_state->dirty_custom_time = FALSE;
    plugin->dialog_state->dirty_timeout = FALSE;
    plugin->dialog_state->dirty_fixed_width = FALSE;
    plugin->dialog_state->dirty_right_icon = FALSE;
    plugin->dialog_state->dirty_align = FALSE;
    DEBUG_TRACE("settings_dialog_reset_dirty: all dirty flags reset");
}

void settings_dialog_update_ui(AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->dialog_state || plugin->is_disposing) return;
    SettingsDialogState *state = plugin->dialog_state;
    state->syncing_ui = TRUE;
    
    if (!plugin->profiles || plugin->profiles->len == 0) {
        DEBUG_TRACE("settings_dialog_update_ui: No profiles loaded, creating fallback");
        create_fallback_profiles(plugin);
    }
    
    gboolean show_battery = !plugin->no_battery;
    DEBUG_TRACE("settings_dialog_update_ui: no_battery = %d, show_battery = %d", plugin->no_battery, show_battery);
    
    if (state->check_ac) {
        gtk_widget_set_visible(GTK_WIDGET(state->check_ac), show_battery);
        gtk_widget_set_sensitive(GTK_WIDGET(state->check_ac), show_battery && plugin->asusd_state == ASUSD_STATE_AVAILABLE);
    }
    if (state->check_battery) {
        gtk_widget_set_visible(GTK_WIDGET(state->check_battery), show_battery);
        gtk_widget_set_sensitive(GTK_WIDGET(state->check_battery), show_battery && plugin->asusd_state == ASUSD_STATE_AVAILABLE);
    }
    if (state->combo_ac) {
        gtk_widget_set_visible(GTK_WIDGET(state->combo_ac), show_battery);
        gtk_widget_set_sensitive(GTK_WIDGET(state->combo_ac), show_battery && plugin->auto_switch_ac_enabled && plugin->asusd_state == ASUSD_STATE_AVAILABLE);
    }
    if (state->combo_battery) {
        gtk_widget_set_visible(GTK_WIDGET(state->combo_battery), show_battery);
        gtk_widget_set_sensitive(GTK_WIDGET(state->combo_battery), show_battery && plugin->auto_switch_battery_enabled && plugin->asusd_state == ASUSD_STATE_AVAILABLE);
    }
    if (state->limit_check) {
        gtk_widget_set_visible(GTK_WIDGET(state->limit_check), show_battery);
        gtk_widget_set_sensitive(GTK_WIDGET(state->limit_check), show_battery && plugin->asusd_state == ASUSD_STATE_AVAILABLE);
    }
    
    GtkWidget *auto_frame = NULL;
    if (state->check_ac) {
        GtkWidget *check_parent = gtk_widget_get_parent(GTK_WIDGET(state->check_ac));
        if (check_parent) {
            GtkWidget *frame_parent = gtk_widget_get_parent(check_parent);
            if (frame_parent) {
                GtkWidget *hbox_parent = gtk_widget_get_parent(frame_parent);
                if (hbox_parent) {
                    GtkWidget *vbox_parent = gtk_widget_get_parent(hbox_parent);
                    if (vbox_parent) {
                        auto_frame = gtk_widget_get_parent(vbox_parent);
                    }
                }
            }
        }
    }
    if (auto_frame) {
        gtk_widget_set_visible(auto_frame, show_battery);
        DEBUG_TRACE("settings_dialog_update_ui: auto_frame visibility = %d", show_battery);
    }
    
    GtkWidget *limit_frame = NULL;
    if (state->limit_check) {
        GtkWidget *limit_parent = gtk_widget_get_parent(GTK_WIDGET(state->limit_check));
        if (limit_parent) {
            limit_frame = gtk_widget_get_parent(limit_parent);
        }
    }
    if (limit_frame) {
        gtk_widget_set_visible(limit_frame, show_battery);
        DEBUG_TRACE("settings_dialog_update_ui: limit_frame visibility = %d", show_battery);
    }
    
    if (state->check_ac && !state->dirty_ac_enabled)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->check_ac), plugin->auto_switch_ac_enabled);
    if (state->check_battery && !state->dirty_battery_enabled)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->check_battery), plugin->auto_switch_battery_enabled);
    if (state->limit_check && !state->dirty_limit)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->limit_check), plugin->battery_limit_enabled);
    
    if (state->hide_icon_check)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->hide_icon_check), plugin->hide_icon);
    if (state->hide_text_check)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->hide_text_check), plugin->hide_text);
    if (state->fixed_width_check && !state->dirty_fixed_width) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->fixed_width_check), plugin->fixed_width_enabled);
    }
    if (state->fixed_width_entry && !state->dirty_fixed_width) {
        gchar *text = g_strdup_printf("%u", plugin->fixed_width_value);
        gtk_entry_set_text(GTK_ENTRY(state->fixed_width_entry), text);
        g_free(text);
        gtk_widget_set_sensitive(state->fixed_width_entry, plugin->fixed_width_enabled);
    }
    if (state->right_icon_check && !state->dirty_right_icon) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->right_icon_check), plugin->right_icon);
    }
    if (state->align_combo && !state->dirty_align) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(state->align_combo), plugin->align);
    }
    
    if (state->notifications_check)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->notifications_check), plugin->hide_notifications);
    
    gboolean notif_hidden = plugin->hide_notifications;
    if (state->antiflapping_check && !state->dirty_antiflapping) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->antiflapping_check), plugin->enable_antiflapping);
        gtk_widget_set_sensitive(GTK_WIDGET(state->antiflapping_check), !notif_hidden);
    }
    if (state->custom_time_check && !state->dirty_custom_time) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->custom_time_check), plugin->custom_time_enabled);
        gtk_widget_set_sensitive(GTK_WIDGET(state->custom_time_check), !notif_hidden && plugin->enable_antiflapping);
    }
    if (state->custom_time_entry && !state->dirty_timeout) {
        gchar *text = g_strdup_printf("%u", plugin->custom_timeout_ms);
        gtk_entry_set_text(GTK_ENTRY(state->custom_time_entry), text);
        g_free(text);
        gtk_widget_set_sensitive(state->custom_time_entry, !notif_hidden && plugin->custom_time_enabled && plugin->enable_antiflapping);
    }
    
    if (state->custom_time_error_label) {
        gtk_widget_set_visible(GTK_WIDGET(state->custom_time_error_label), FALSE);
    }
    
    if (state->combo_ac && !state->dirty_ac_profile && plugin->auto_switch_ac_profile) {
        guint32 enum_val;
        if (profile_enum_from_name(plugin, plugin->auto_switch_ac_profile, &enum_val)) {
            int index = -1;
            GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(state->combo_ac));
            if (model) {
                GtkTreeIter iter;
                gint current_idx = 0;
                if (gtk_tree_model_get_iter_first(model, &iter)) {
                    do {
                        ProfileSettings *settings = g_ptr_array_index(plugin->profiles, current_idx);
                        if (settings && settings->enum_value == enum_val) {
                            index = current_idx;
                            break;
                        }
                        current_idx++;
                    } while (gtk_tree_model_iter_next(model, &iter));
                }
            }
            if (index >= 0) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(state->combo_ac), index);
            }
        }
    }
    
    if (state->combo_battery && !state->dirty_battery_profile && plugin->auto_switch_battery_profile) {
        guint32 enum_val;
        if (profile_enum_from_name(plugin, plugin->auto_switch_battery_profile, &enum_val)) {
            int index = -1;
            GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(state->combo_battery));
            if (model) {
                GtkTreeIter iter;
                gint current_idx = 0;
                if (gtk_tree_model_get_iter_first(model, &iter)) {
                    do {
                        ProfileSettings *settings = g_ptr_array_index(plugin->profiles, current_idx);
                        if (settings && settings->enum_value == enum_val) {
                            index = current_idx;
                            break;
                        }
                        current_idx++;
                    } while (gtk_tree_model_iter_next(model, &iter));
                }
            }
            if (index >= 0) {
                gtk_combo_box_set_active(GTK_COMBO_BOX(state->combo_battery), index);
            }
        }
    }
    
    state->syncing_ui = FALSE;
}

void settings_dialog_sync_from_asusd(AsusdBatteryPlugin *plugin, gboolean keep_dirty) {
    if (!plugin || !plugin->dialog_state || plugin->is_disposing) return;
    
    if (plugin->no_battery) {
        DEBUG_TRACE("settings_dialog_sync_from_asusd: no_battery enabled, skipping ASUSD battery settings");
        settings_dialog_update_ui(plugin);
        if (!keep_dirty) {
            settings_dialog_reset_dirty(plugin);
        }
        return;
    }
    
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) {
        DEBUG_TRACE("settings_dialog_sync_from_asusd: ASUSD not available, using cached values");
        settings_dialog_update_ui(plugin);
        if (!keep_dirty) {
            settings_dialog_reset_dirty(plugin);
        }
        return;
    }
    
    guint dialog_id = plugin->dialog_state->dialog_id;
    plugin->dialog_state->syncing_ui = TRUE;
    DEBUG_TRACE("settings_dialog_sync_from_asusd: Loading settings from ASUSD");
    
    if (plugin->dialog_state->custom_time_error_label) {
        gtk_widget_set_visible(GTK_WIDGET(plugin->dialog_state->custom_time_error_label), FALSE);
    }
    
    AsyncCallContext *ctx = async_call_context_new(plugin, NULL, NULL,
                                                   (GAsyncReadyCallback)on_dialog_property_loaded, NULL, NULL);
    ctx->dialog_id = dialog_id;
    ctx->is_dialog_callback = TRUE;
    ctx->user_data = GINT_TO_POINTER(keep_dirty ? 1000 : 0);
    
    asusd_get_property_async(plugin, "ChangePlatformProfileOnAc",
                            (GAsyncReadyCallback)on_dialog_property_loaded, ctx);
}

void on_dialog_property_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsyncCallContext *ctx = (AsyncCallContext*)user_data;
    if (!ctx) return;
    
    AsusdBatteryPlugin *plugin = async_call_context_get_plugin_ref(ctx);
    if (!plugin || plugin->is_disposing) {
        DEBUG_TRACE("on_dialog_property_loaded: plugin destroyed or disposing");
        if (plugin) g_object_unref(plugin);
        async_call_context_free(ctx);
        return;
    }
    
    guint dialog_id = ctx->dialog_id;
    int keep_dirty_flag = GPOINTER_TO_INT(ctx->user_data);
    int step = keep_dirty_flag >= 1000 ? keep_dirty_flag - 1000 : keep_dirty_flag;
    gboolean keep_dirty = (keep_dirty_flag >= 1000);
    
    if (!plugin->dialog_state) {
        DEBUG_TRACE("on_dialog_property_loaded: dialog_state is NULL, discarding");
        async_call_context_free(ctx);
        g_object_unref(plugin);
        return;
    }
    
    if (!is_dialog_valid(plugin, dialog_id)) {
        DEBUG_TRACE("on_dialog_property_loaded: dialog changed or destroyed (id %u)", dialog_id);
        async_call_context_free(ctx);
        g_object_unref(plugin);
        return;
    }
    
    if (!res) {
        DEBUG_TRACE("on_dialog_property_loaded: operation failed with NULL result");
        async_call_context_free(ctx);
        g_object_unref(plugin);
        return;
    }
    
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    
    switch (step) {
        case 0: {
            if (error) {
                DEBUG_TRACE("Failed to get ChangePlatformProfileOnAc: %s", error->message);
                g_error_free(error);
                plugin->auto_switch_ac_enabled = FALSE;
                if (is_dialog_valid(plugin, dialog_id) && plugin->dialog_state->check_ac && !plugin->dialog_state->dirty_ac_enabled)
                    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->check_ac), FALSE);
            } else if (result) {
                GVariant *value = NULL;
                g_variant_get(result, "(v)", &value);
                if (value) {
                    if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
                        GVariant *inner = g_variant_get_variant(value);
                        g_variant_unref(value);
                        value = inner;
                    }
                    if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
                        gboolean ac_enabled;
                        g_variant_get(value, "b", &ac_enabled);
                        plugin->auto_switch_ac_enabled = ac_enabled;
                        DEBUG_TRACE("ChangePlatformProfileOnAc = %d", ac_enabled);
                        if (is_dialog_valid(plugin, dialog_id) && plugin->dialog_state->check_ac && !plugin->dialog_state->dirty_ac_enabled)
                            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->check_ac), ac_enabled);
                    }
                    g_variant_unref(value);
                }
                g_variant_unref(result);
            }
            
            if (is_dialog_valid(plugin, dialog_id))
                plugin->dialog_state->syncing_ui = TRUE;
            
            ctx->user_data = GINT_TO_POINTER(keep_dirty ? 1001 : 1);
            if (!asusd_get_property_async(plugin, "ChangePlatformProfileOnBattery",
                                          (GAsyncReadyCallback)on_dialog_property_loaded, ctx)) {
                DEBUG_TRACE("Failed to start async operation for ChangePlatformProfileOnBattery");
                async_call_context_free(ctx);
                g_object_unref(plugin);
                return;
            }
            g_object_unref(plugin);
            return;
        }
        case 1: {
            if (error) {
                DEBUG_TRACE("Failed to get ChangePlatformProfileOnBattery: %s", error->message);
                g_error_free(error);
                plugin->auto_switch_battery_enabled = FALSE;
                if (is_dialog_valid(plugin, dialog_id) && plugin->dialog_state->check_battery && !plugin->dialog_state->dirty_battery_enabled)
                    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->check_battery), FALSE);
            } else if (result) {
                GVariant *value = NULL;
                g_variant_get(result, "(v)", &value);
                if (value) {
                    if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
                        GVariant *inner = g_variant_get_variant(value);
                        g_variant_unref(value);
                        value = inner;
                    }
                    if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
                        gboolean battery_enabled;
                        g_variant_get(value, "b", &battery_enabled);
                        plugin->auto_switch_battery_enabled = battery_enabled;
                        DEBUG_TRACE("ChangePlatformProfileOnBattery = %d", battery_enabled);
                        if (is_dialog_valid(plugin, dialog_id) && plugin->dialog_state->check_battery && !plugin->dialog_state->dirty_battery_enabled)
                            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->check_battery), battery_enabled);
                    }
                    g_variant_unref(value);
                }
                g_variant_unref(result);
            }
            
            if (is_dialog_valid(plugin, dialog_id))
                plugin->dialog_state->syncing_ui = TRUE;
            
            ctx->user_data = GINT_TO_POINTER(keep_dirty ? 1002 : 2);
            if (!asusd_get_property_async(plugin, "PlatformProfileOnAc",
                                          (GAsyncReadyCallback)on_dialog_property_loaded, ctx)) {
                DEBUG_TRACE("Failed to start async operation for PlatformProfileOnAc");
                async_call_context_free(ctx);
                g_object_unref(plugin);
                return;
            }
            g_object_unref(plugin);
            return;
        }
        case 2: {
            const char *default_profile = "balanced";
            if (plugin->profiles && plugin->profiles->len > 0) {
                ProfileSettings *settings = g_ptr_array_index(plugin->profiles, 0);
                if (settings->default_name) default_profile = settings->default_name;
            }
            
            if (error) {
                DEBUG_TRACE("Failed to get PlatformProfileOnAc: %s", error->message);
                g_error_free(error);
                g_free(plugin->auto_switch_ac_profile);
                plugin->auto_switch_ac_profile = g_strdup(default_profile);
            } else if (result) {
                GVariant *value = NULL;
                g_variant_get(result, "(v)", &value);
                if (value) {
                    if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
                        GVariant *inner = g_variant_get_variant(value);
                        g_variant_unref(value);
                        value = inner;
                    }
                    if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32)) {
                        guint32 enum_val;
                        g_variant_get(value, "u", &enum_val);
                        const gchar *name = profile_name_from_enum(plugin, enum_val);
                        g_free(plugin->auto_switch_ac_profile);
                        plugin->auto_switch_ac_profile = g_strdup(name);
                        DEBUG_TRACE("PlatformProfileOnAc = %s", name);
                        if (is_dialog_valid(plugin, dialog_id) && plugin->dialog_state->combo_ac && !plugin->dialog_state->dirty_ac_profile) {
                            int index = -1;
                            GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(plugin->dialog_state->combo_ac));
                            if (model) {
                                GtkTreeIter iter;
                                gint current_idx = 0;
                                if (gtk_tree_model_get_iter_first(model, &iter)) {
                                    do {
                                        ProfileSettings *settings = g_ptr_array_index(plugin->profiles, current_idx);
                                        if (settings && settings->enum_value == enum_val) {
                                            index = current_idx;
                                            break;
                                        }
                                        current_idx++;
                                    } while (gtk_tree_model_iter_next(model, &iter));
                                }
                            }
                            if (index >= 0) {
                                gtk_combo_box_set_active(GTK_COMBO_BOX(plugin->dialog_state->combo_ac), index);
                            }
                        }
                    }
                    g_variant_unref(value);
                }
                g_variant_unref(result);
            }
            
            if (is_dialog_valid(plugin, dialog_id))
                plugin->dialog_state->syncing_ui = TRUE;
            
            ctx->user_data = GINT_TO_POINTER(keep_dirty ? 1003 : 3);
            if (!asusd_get_property_async(plugin, "PlatformProfileOnBattery",
                                          (GAsyncReadyCallback)on_dialog_property_loaded, ctx)) {
                DEBUG_TRACE("Failed to start async operation for PlatformProfileOnBattery");
                async_call_context_free(ctx);
                g_object_unref(plugin);
                return;
            }
            g_object_unref(plugin);
            return;
        }
        case 3: {
            const char *default_profile = "balanced";
            if (plugin->profiles && plugin->profiles->len > 0) {
                ProfileSettings *settings = g_ptr_array_index(plugin->profiles, 0);
                if (settings->default_name) default_profile = settings->default_name;
            }
            
            if (error) {
                DEBUG_TRACE("Failed to get PlatformProfileOnBattery: %s", error->message);
                g_error_free(error);
                g_free(plugin->auto_switch_battery_profile);
                plugin->auto_switch_battery_profile = g_strdup(default_profile);
            } else if (result) {
                GVariant *value = NULL;
                g_variant_get(result, "(v)", &value);
                if (value) {
                    if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
                        GVariant *inner = g_variant_get_variant(value);
                        g_variant_unref(value);
                        value = inner;
                    }
                    if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32)) {
                        guint32 enum_val;
                        g_variant_get(value, "u", &enum_val);
                        const gchar *name = profile_name_from_enum(plugin, enum_val);
                        g_free(plugin->auto_switch_battery_profile);
                        plugin->auto_switch_battery_profile = g_strdup(name);
                        DEBUG_TRACE("PlatformProfileOnBattery = %s", name);
                        if (is_dialog_valid(plugin, dialog_id) && plugin->dialog_state->combo_battery && !plugin->dialog_state->dirty_battery_profile) {
                            int index = -1;
                            GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(plugin->dialog_state->combo_battery));
                            if (model) {
                                GtkTreeIter iter;
                                gint current_idx = 0;
                                if (gtk_tree_model_get_iter_first(model, &iter)) {
                                    do {
                                        ProfileSettings *settings = g_ptr_array_index(plugin->profiles, current_idx);
                                        if (settings && settings->enum_value == enum_val) {
                                            index = current_idx;
                                            break;
                                        }
                                        current_idx++;
                                    } while (gtk_tree_model_iter_next(model, &iter));
                                }
                            }
                            if (index >= 0) {
                                gtk_combo_box_set_active(GTK_COMBO_BOX(plugin->dialog_state->combo_battery), index);
                            }
                        }
                    }
                    g_variant_unref(value);
                }
                g_variant_unref(result);
            }
            
            if (is_dialog_valid(plugin, dialog_id))
                plugin->dialog_state->syncing_ui = TRUE;
            
            ctx->user_data = GINT_TO_POINTER(keep_dirty ? 1004 : 4);
            if (!asusd_get_property_async(plugin, "ChargeControlEndThreshold",
                                          (GAsyncReadyCallback)on_dialog_property_loaded, ctx)) {
                DEBUG_TRACE("Failed to start async operation for ChargeControlEndThreshold");
                async_call_context_free(ctx);
                g_object_unref(plugin);
                return;
            }
            g_object_unref(plugin);
            return;
        }
        case 4: {
            if (error) {
                DEBUG_TRACE("Failed to get ChargeControlEndThreshold: %s", error->message);
                g_error_free(error);
                plugin->current_battery_limit = 100;
                plugin->battery_limit_enabled = FALSE;
                if (is_dialog_valid(plugin, dialog_id) && plugin->dialog_state->limit_check && !plugin->dialog_state->dirty_limit)
                    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->limit_check), FALSE);
            } else if (result) {
                GVariant *value = NULL;
                g_variant_get(result, "(v)", &value);
                if (value) {
                    if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
                        GVariant *inner = g_variant_get_variant(value);
                        g_variant_unref(value);
                        value = inner;
                    }
                    if (g_variant_is_of_type(value, G_VARIANT_TYPE_BYTE)) {
                        guint8 limit;
                        g_variant_get(value, "y", &limit);
                        plugin->current_battery_limit = limit;
                        plugin->battery_limit_enabled = (limit == 80);
                        DEBUG_TRACE("ChargeControlEndThreshold = %d", limit);
                        if (is_dialog_valid(plugin, dialog_id) && plugin->dialog_state->limit_check && !plugin->dialog_state->dirty_limit)
                            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->limit_check), (limit == 80));
                    }
                    g_variant_unref(value);
                }
                g_variant_unref(result);
            }
            
            if (is_dialog_valid(plugin, dialog_id))
                plugin->dialog_state->syncing_ui = FALSE;
            
            if (!keep_dirty) {
                settings_dialog_reset_dirty(plugin);
                DEBUG_TRACE("Dialog settings loaded completely, dirty flags reset");
            } else {
                DEBUG_TRACE("Dialog settings loaded completely, keeping dirty flags");
            }
            
            async_call_context_free(ctx);
            g_object_unref(plugin);
            return;
        }
        default:
            break;
    }
    
    async_call_context_free(ctx);
    g_object_unref(plugin);
}

/* ========== Создание диалога ========== */

void create_settings_dialog(AsusdBatteryPlugin *plugin) {
    if (!plugin || plugin->is_disposing) return;
    
    if (plugin->settings_dialog_open && plugin->dialog_state && plugin->dialog_state->dialog) {
        DEBUG_TRACE("create_settings_dialog: dialog already open");
        if (GTK_IS_WIDGET(plugin->dialog_state->dialog) && 
            gtk_widget_get_parent(plugin->dialog_state->dialog) != NULL) {
            gtk_window_present(GTK_WINDOW(plugin->dialog_state->dialog));
            return;
        } else {
            g_free(plugin->dialog_state);
            plugin->dialog_state = NULL;
            plugin->settings_dialog_open = FALSE;
        }
    }
    
    if (plugin->dialog_state) {
        DEBUG_TRACE("create_settings_dialog: cleaning up stale dialog_state");
        g_free(plugin->dialog_state);
        plugin->dialog_state = NULL;
    }
    
    GtkWidget *dialog;
    GtkWidget *content_area;
    GtkWidget *vbox;
    GtkWidget *hide_icon_check_widget;
    GtkWidget *hide_text_check_widget;
    GtkWidget *notifications_check_widget;
    GtkWidget *limit_check_widget;
    GtkWidget *label;
    GtkWidget *entry_name;
    GtkWidget *entry_icon;
    GtkWidget *grid;
    GtkWidget *frame_ac;
    GtkWidget *frame_battery;
    GtkWidget *hbox_ac;
    GtkWidget *hbox_battery;
    GtkWidget *combo_ac;
    GtkWidget *combo_battery;
    GtkListStore *store_ac;
    GtkListStore *store_battery;
    GtkCellRenderer *renderer;
    GtkWidget *auto_frame;
    GtkWidget *auto_vbox;
    GtkWidget *hide_frame;
    GtkWidget *hide_vbox;
    GtkWidget *ac_hbox;
    GtkWidget *one_shot_label;
    GtkWidget *one_shot_button;
    GtkWidget *button_box;
    GtkWidget *apply_button;
    GtkWidget *close_button;
    GtkWidget *options_frame;
    GtkWidget *options_vbox;
    GtkWidget *options_hbox;
    GtkWidget *hide_label;
    GtkWidget *antiflapping_frame;
    GtkWidget *antiflapping_vbox;
    GtkWidget *ms_label;
    GtkWidget *antiflapping_hbox;
    GtkWidget *hide_notif_hbox;
    int row = 0;
    
    DEBUG_TRACE("=== create_settings_dialog: OPENING ===");
    plugin->settings_dialog_open = TRUE;
    plugin->saving_settings = FALSE;
    
    plugin->dialog_id_counter++;
    guint current_dialog_id = plugin->dialog_id_counter;
    
    plugin->dialog_state = g_new0(SettingsDialogState, 1);
    SettingsDialogState *state = plugin->dialog_state;
    state->dialog_id = current_dialog_id;
    settings_dialog_reset_dirty(plugin);
    
    if (!plugin->profiles || plugin->profiles->len == 0) {
        DEBUG_TRACE("create_settings_dialog: No profiles loaded, creating fallback");
        create_fallback_profiles(plugin);
    }
    
    load_settings(plugin);
    
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) {
        DEBUG_TRACE("ASUSD not available, trying to initialize...");
        asusd_init_async(plugin);
    }
    
    dialog = gtk_dialog_new_with_buttons(_("Power Profile Settings"),
                                        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(plugin->plugin))),
                                        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                        NULL, NULL);
    if (!dialog) { DEBUG_WARN("Failed to create settings dialog"); plugin->settings_dialog_open = FALSE; g_free(plugin->dialog_state); plugin->dialog_state = NULL; return; }
    state->dialog = dialog;
    g_object_set_data(G_OBJECT(dialog), "plugin", plugin);
    gtk_window_set_icon_name(GTK_WINDOW(dialog), "emblem-system");
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    if (!content_area) { gtk_widget_destroy(dialog); plugin->settings_dialog_open = FALSE; g_free(plugin->dialog_state); plugin->dialog_state = NULL; return; }
    gtk_container_set_border_width(GTK_CONTAINER(content_area), 10);
    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    if (!vbox) { gtk_widget_destroy(dialog); plugin->settings_dialog_open = FALSE; g_free(plugin->dialog_state); plugin->dialog_state = NULL; return; }
    gtk_container_add(GTK_CONTAINER(content_area), vbox);
    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_halign(main_vbox, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), main_vbox, TRUE, TRUE, 0);
    
    /* Auto switch profiles */
    auto_frame = gtk_frame_new(_("Auto switch profiles"));
    gtk_box_pack_start(GTK_BOX(main_vbox), auto_frame, FALSE, FALSE, 0);
    auto_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(auto_vbox), 5);
    gtk_container_add(GTK_CONTAINER(auto_frame), auto_vbox);
    ac_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_widget_set_halign(ac_hbox, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(auto_vbox), ac_hbox, FALSE, FALSE, 0);
    
    frame_ac = gtk_frame_new(_("On AC"));
    gtk_box_pack_start(GTK_BOX(ac_hbox), frame_ac, FALSE, FALSE, 0);
    hbox_ac = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(hbox_ac), 5);
    gtk_container_add(GTK_CONTAINER(frame_ac), hbox_ac);
    GtkWidget *check_ac = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check_ac), plugin->auto_switch_ac_enabled);
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) gtk_widget_set_sensitive(check_ac, FALSE);
    state->check_ac = check_ac;
    g_object_set_data(G_OBJECT(dialog), "check_ac", check_ac);
    gtk_box_pack_start(GTK_BOX(hbox_ac), check_ac, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(check_ac), "toggled", G_CALLBACK(on_any_setting_changed), plugin);
    
    store_ac = gtk_list_store_new(1, G_TYPE_STRING);
    combo_ac = gtk_combo_box_new_with_model(GTK_TREE_MODEL(store_ac));
    renderer = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combo_ac), renderer, TRUE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(combo_ac), renderer, "text", 0, NULL);
    gtk_widget_set_size_request(combo_ac, 100, -1);
    if (plugin->profiles) {
        GtkTreeIter iter;
        int active_index = 0;
        for (guint i = 0; i < plugin->profiles->len; i++) {
            ProfileSettings *settings = g_ptr_array_index(plugin->profiles, i);
            const char *name = settings->name && strlen(settings->name) > 0 ? settings->name : settings->default_name;
            gtk_list_store_append(store_ac, &iter);
            gtk_list_store_set(store_ac, &iter, 0, name, -1);
            if (plugin->auto_switch_ac_profile) {
                guint32 enum_val;
                if (profile_enum_from_name(plugin, plugin->auto_switch_ac_profile, &enum_val)) {
                    if (settings->enum_value == enum_val) {
                        active_index = i;
                    }
                }
            }
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo_ac), active_index);
    }
    gtk_widget_set_sensitive(combo_ac, plugin->auto_switch_ac_enabled && plugin->asusd_state == ASUSD_STATE_AVAILABLE);
    state->combo_ac = combo_ac;
    g_object_set_data(G_OBJECT(dialog), "combo_ac", combo_ac);
    gtk_box_pack_start(GTK_BOX(hbox_ac), combo_ac, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(combo_ac), "changed", G_CALLBACK(on_any_setting_changed), plugin);
    g_signal_connect(G_OBJECT(check_ac), "toggled", G_CALLBACK(on_auto_switch_toggled), dialog);
    
    frame_battery = gtk_frame_new(_("On Battery"));
    gtk_box_pack_start(GTK_BOX(ac_hbox), frame_battery, FALSE, FALSE, 0);
    hbox_battery = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(hbox_battery), 5);
    gtk_container_add(GTK_CONTAINER(frame_battery), hbox_battery);
    GtkWidget *check_battery = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check_battery), plugin->auto_switch_battery_enabled);
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) gtk_widget_set_sensitive(check_battery, FALSE);
    state->check_battery = check_battery;
    g_object_set_data(G_OBJECT(dialog), "check_battery", check_battery);
    gtk_box_pack_start(GTK_BOX(hbox_battery), check_battery, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(check_battery), "toggled", G_CALLBACK(on_any_setting_changed), plugin);
    
    store_battery = gtk_list_store_new(1, G_TYPE_STRING);
    combo_battery = gtk_combo_box_new_with_model(GTK_TREE_MODEL(store_battery));
    renderer = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combo_battery), renderer, TRUE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(combo_battery), renderer, "text", 0, NULL);
    gtk_widget_set_size_request(combo_battery, 100, -1);
    if (plugin->profiles) {
        GtkTreeIter iter;
        int active_index = 0;
        for (guint i = 0; i < plugin->profiles->len; i++) {
            ProfileSettings *settings = g_ptr_array_index(plugin->profiles, i);
            const char *name = settings->name && strlen(settings->name) > 0 ? settings->name : settings->default_name;
            gtk_list_store_append(store_battery, &iter);
            gtk_list_store_set(store_battery, &iter, 0, name, -1);
            if (plugin->auto_switch_battery_profile) {
                guint32 enum_val;
                if (profile_enum_from_name(plugin, plugin->auto_switch_battery_profile, &enum_val)) {
                    if (settings->enum_value == enum_val) {
                        active_index = i;
                    }
                }
            }
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo_battery), active_index);
    }
    gtk_widget_set_sensitive(combo_battery, plugin->auto_switch_battery_enabled && plugin->asusd_state == ASUSD_STATE_AVAILABLE);
    state->combo_battery = combo_battery;
    g_object_set_data(G_OBJECT(dialog), "combo_battery", combo_battery);
    gtk_box_pack_start(GTK_BOX(hbox_battery), combo_battery, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(combo_battery), "changed", G_CALLBACK(on_any_setting_changed), plugin);
    g_signal_connect(G_OBJECT(check_battery), "toggled", G_CALLBACK(on_auto_switch_toggled), dialog);
    
    /* Battery charge limit */
    GtkWidget *limit_frame = gtk_frame_new(_("Charge limit"));
    gtk_box_pack_start(GTK_BOX(main_vbox), limit_frame, FALSE, FALSE, 0);
    GtkWidget *limit_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(limit_vbox), 5);
    gtk_container_add(GTK_CONTAINER(limit_frame), limit_vbox);
    
    /* Одна строка: Limit to 80% + Full charge once */
    GtkWidget *limit_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(limit_hbox, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(limit_vbox), limit_hbox, FALSE, FALSE, 0);
    
    limit_check_widget = gtk_check_button_new_with_label(_("Limit to 80%"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(limit_check_widget), plugin->battery_limit_enabled);
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) gtk_widget_set_sensitive(limit_check_widget, FALSE);
    state->limit_check = limit_check_widget;
    g_object_set_data(G_OBJECT(dialog), "limit_check", limit_check_widget);
    gtk_box_pack_start(GTK_BOX(limit_hbox), limit_check_widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(limit_check_widget), "toggled", G_CALLBACK(on_any_setting_changed), plugin);
    
    one_shot_label = gtk_label_new(_("Full charge once:"));
    gtk_box_pack_start(GTK_BOX(limit_hbox), one_shot_label, FALSE, FALSE, 0);
    one_shot_button = gtk_button_new_with_label(_("Start"));
    g_signal_connect(G_OBJECT(one_shot_button), "clicked", G_CALLBACK(on_one_shot_clicked), dialog);
    gtk_box_pack_start(GTK_BOX(limit_hbox), one_shot_button, FALSE, FALSE, 0);
    
    /* Profile names and icons */
    hide_frame = gtk_frame_new(_("Profile names and icons"));
    gtk_box_pack_start(GTK_BOX(main_vbox), hide_frame, FALSE, FALSE, 0);
    hide_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(hide_vbox), 5);
    gtk_container_add(GTK_CONTAINER(hide_frame), hide_vbox);
    gtk_widget_set_halign(hide_vbox, GTK_ALIGN_CENTER);
    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_box_pack_start(GTK_BOX(hide_vbox), grid, FALSE, FALSE, 0);
    row = 0;
    if (plugin->profiles) {
        for (guint i = 0; i < plugin->profiles->len; i++) {
            ProfileSettings *settings = g_ptr_array_index(plugin->profiles, i);
            if (!settings) continue;
            gchar *display_name = g_strdup(settings->default_name);
            if (display_name[0] >= 'a' && display_name[0] <= 'z') display_name[0] = g_ascii_toupper(display_name[0]);
            label = gtk_label_new(display_name);
            gtk_label_set_xalign(GTK_LABEL(label), 0.0);
            gtk_widget_set_size_request(label, 80, -1);
            gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
            g_free(display_name);
            entry_name = gtk_entry_new();
            if (settings->name && strlen(settings->name) > 0) gtk_entry_set_text(GTK_ENTRY(entry_name), settings->name);
            else gtk_entry_set_placeholder_text(GTK_ENTRY(entry_name), settings->default_name);
            gtk_widget_set_size_request(entry_name, 120, -1);
            g_object_set_data(G_OBJECT(dialog), g_strdup_printf("entry_name_%d", settings->enum_value), entry_name);
            gtk_grid_attach(GTK_GRID(grid), entry_name, 1, row, 1, 1);
            g_signal_connect(G_OBJECT(entry_name), "changed", G_CALLBACK(on_any_setting_changed), plugin);
            
            entry_icon = gtk_entry_new();
            
            const char *default_icon = NULL;
            if (g_strcmp0(settings->default_name, "performance") == 0) {
                default_icon = "battery-full-symbolic";
            } else if (g_strcmp0(settings->default_name, "balanced") == 0) {
                default_icon = "battery-good-symbolic";
            } else if (g_strcmp0(settings->default_name, "quiet") == 0) {
                default_icon = "battery-low-symbolic";
            }
            
            if (settings->icon && strlen(settings->icon) > 0 && default_icon && 
                g_strcmp0(settings->icon, default_icon) != 0) {
                gtk_entry_set_text(GTK_ENTRY(entry_icon), settings->icon);
            } else {
                gtk_entry_set_text(GTK_ENTRY(entry_icon), "");
            }
            
            gtk_entry_set_placeholder_text(GTK_ENTRY(entry_icon), _("icon name"));
            gtk_widget_set_size_request(entry_icon, 120, -1);
            g_object_set_data(G_OBJECT(dialog), g_strdup_printf("entry_icon_%d", settings->enum_value), entry_icon);
            gtk_grid_attach(GTK_GRID(grid), entry_icon, 2, row, 1, 1);
            g_signal_connect(G_OBJECT(entry_icon), "changed", G_CALLBACK(on_any_setting_changed), plugin);
            row++;
        }
    }
    
    /* ===== Display options ===== */
    options_frame = gtk_frame_new(_("Display options"));
    gtk_box_pack_start(GTK_BOX(main_vbox), options_frame, FALSE, FALSE, 0);
    options_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(options_vbox), 5);
    gtk_container_add(GTK_CONTAINER(options_frame), options_vbox);
    
    /* Первая строка: Hide: Icon, Text, Elements align */
    options_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_halign(options_hbox, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(options_vbox), options_hbox, FALSE, FALSE, 0);
    
    hide_label = gtk_label_new(_("Hide:"));
    gtk_box_pack_start(GTK_BOX(options_hbox), hide_label, FALSE, FALSE, 0);
    
    hide_icon_check_widget = gtk_check_button_new_with_label(_("Icon"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hide_icon_check_widget), plugin->hide_icon);
    gtk_box_pack_start(GTK_BOX(options_hbox), hide_icon_check_widget, FALSE, FALSE, 0);
    state->hide_icon_check = hide_icon_check_widget;
    g_object_set_data(G_OBJECT(dialog), "hide_icon_check", hide_icon_check_widget);
    g_signal_connect(G_OBJECT(hide_icon_check_widget), "toggled", G_CALLBACK(on_hide_toggle), plugin);
    g_signal_connect(G_OBJECT(hide_icon_check_widget), "toggled", G_CALLBACK(on_any_setting_changed), plugin);
    
    hide_text_check_widget = gtk_check_button_new_with_label(_("Text"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hide_text_check_widget), plugin->hide_text);
    gtk_box_pack_start(GTK_BOX(options_hbox), hide_text_check_widget, FALSE, FALSE, 0);
    state->hide_text_check = hide_text_check_widget;
    g_object_set_data(G_OBJECT(dialog), "hide_text_check", hide_text_check_widget);
    g_signal_connect(G_OBJECT(hide_text_check_widget), "toggled", G_CALLBACK(on_hide_toggle), plugin);
    g_signal_connect(G_OBJECT(hide_text_check_widget), "toggled", G_CALLBACK(on_any_setting_changed), plugin);
    
    /* Промежуток */
    GtkWidget *spacer = gtk_label_new("  ");
    gtk_box_pack_start(GTK_BOX(options_hbox), spacer, FALSE, FALSE, 0);
    
    /* Elements align */
    GtkWidget *align_label = gtk_label_new(_("Elements align:"));
    gtk_box_pack_start(GTK_BOX(options_hbox), align_label, FALSE, FALSE, 0);
    
    GtkWidget *align_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(align_combo), _("Left"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(align_combo), _("Center"));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(align_combo), _("Right"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(align_combo), plugin->align);
    gtk_widget_set_size_request(align_combo, 80, -1);
    gtk_box_pack_start(GTK_BOX(options_hbox), align_combo, FALSE, FALSE, 0);
    state->align_combo = align_combo;
    g_signal_connect(G_OBJECT(align_combo), "changed", G_CALLBACK(on_any_setting_changed), plugin);
    
    /* Вторая строка: Minimal width + Right icon */
    GtkWidget *display_options_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(display_options_hbox, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(options_vbox), display_options_hbox, FALSE, FALSE, 0);
    
    GtkWidget *fixed_width_check = gtk_check_button_new_with_label(_("Minimal width"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(fixed_width_check), plugin->fixed_width_enabled);
    gtk_box_pack_start(GTK_BOX(display_options_hbox), fixed_width_check, FALSE, FALSE, 0);
    state->fixed_width_check = fixed_width_check;
    g_signal_connect(G_OBJECT(fixed_width_check), "toggled", G_CALLBACK(on_fixed_width_toggled), plugin);
    g_signal_connect(G_OBJECT(fixed_width_check), "toggled", G_CALLBACK(on_any_setting_changed), plugin);
    
    GtkWidget *fixed_width_entry = gtk_entry_new();
    gchar *fw_text = g_strdup_printf("%u", plugin->fixed_width_value);
    gtk_entry_set_text(GTK_ENTRY(fixed_width_entry), fw_text);
    g_free(fw_text);
    gtk_entry_set_width_chars(GTK_ENTRY(fixed_width_entry), 3);
    gtk_widget_set_size_request(fixed_width_entry, 40, -1);
    gtk_widget_set_sensitive(fixed_width_entry, plugin->fixed_width_enabled);
    gtk_box_pack_start(GTK_BOX(display_options_hbox), fixed_width_entry, FALSE, FALSE, 0);
    state->fixed_width_entry = fixed_width_entry;
    g_signal_connect(G_OBJECT(fixed_width_entry), "changed", G_CALLBACK(on_any_setting_changed), plugin);
    
    GtkWidget *right_icon_check = gtk_check_button_new_with_label(_("Right icon"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(right_icon_check), plugin->right_icon);
    gtk_box_pack_start(GTK_BOX(display_options_hbox), right_icon_check, FALSE, FALSE, 0);
    state->right_icon_check = right_icon_check;
    g_signal_connect(G_OBJECT(right_icon_check), "toggled", G_CALLBACK(on_any_setting_changed), plugin);
    
    /* ===== Notifications ===== */
    antiflapping_frame = gtk_frame_new(_("Notifications"));
    gtk_box_pack_start(GTK_BOX(main_vbox), antiflapping_frame, FALSE, FALSE, 0);
    
    antiflapping_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(antiflapping_vbox), 5);
    gtk_container_add(GTK_CONTAINER(antiflapping_frame), antiflapping_vbox);
    
    /* Первая строка: Hide notifications */
    hide_notif_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_halign(hide_notif_hbox, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(antiflapping_vbox), hide_notif_hbox, FALSE, FALSE, 0);
    
    notifications_check_widget = gtk_check_button_new_with_label(_("Hide notifications"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(notifications_check_widget), plugin->hide_notifications);
    gtk_box_pack_start(GTK_BOX(hide_notif_hbox), notifications_check_widget, FALSE, FALSE, 0);
    state->notifications_check = notifications_check_widget;
    g_object_set_data(G_OBJECT(dialog), "notifications_check", notifications_check_widget);
    g_signal_connect(G_OBJECT(notifications_check_widget), "toggled", G_CALLBACK(on_notifications_toggled), plugin);
    g_signal_connect(G_OBJECT(notifications_check_widget), "toggled", G_CALLBACK(on_any_setting_changed), plugin);
    
    /* Вторая строка: Anti-flapping + Time */
    gboolean notif_hidden = plugin->hide_notifications;
    antiflapping_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_halign(antiflapping_hbox, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(antiflapping_vbox), antiflapping_hbox, FALSE, FALSE, 0);
    
    GtkWidget *antiflapping_check = gtk_check_button_new_with_label(_("Anti-flapping"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(antiflapping_check), plugin->enable_antiflapping);
    gtk_widget_set_sensitive(antiflapping_check, !notif_hidden);
    state->antiflapping_check = antiflapping_check;
    gtk_box_pack_start(GTK_BOX(antiflapping_hbox), antiflapping_check, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(antiflapping_check), "toggled", G_CALLBACK(on_antiflapping_toggled), plugin);
    g_signal_connect(G_OBJECT(antiflapping_check), "toggled", G_CALLBACK(on_any_setting_changed), plugin);
    
    GtkWidget *custom_time_check = gtk_check_button_new_with_label(_("Time:"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(custom_time_check), plugin->custom_time_enabled);
    gtk_widget_set_sensitive(custom_time_check, !notif_hidden && plugin->enable_antiflapping);
    state->custom_time_check = custom_time_check;
    gtk_box_pack_start(GTK_BOX(antiflapping_hbox), custom_time_check, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(custom_time_check), "toggled", G_CALLBACK(on_custom_time_toggled), plugin);
    g_signal_connect(G_OBJECT(custom_time_check), "toggled", G_CALLBACK(on_any_setting_changed), plugin);
    
    GtkWidget *custom_time_entry = gtk_entry_new();
    gchar *timeout_text = g_strdup_printf("%u", plugin->custom_timeout_ms);
    gtk_entry_set_text(GTK_ENTRY(custom_time_entry), timeout_text);
    g_free(timeout_text);
    char default_timeout_str[8];
    snprintf(default_timeout_str, sizeof(default_timeout_str), "%d", DEFAULT_TIMEOUT_MS);
    gtk_entry_set_placeholder_text(GTK_ENTRY(custom_time_entry), default_timeout_str);
    gtk_entry_set_width_chars(GTK_ENTRY(custom_time_entry), 4);
    gtk_widget_set_size_request(custom_time_entry, 60, -1);
    gtk_widget_set_sensitive(custom_time_entry, !notif_hidden && plugin->custom_time_enabled && plugin->enable_antiflapping);
    state->custom_time_entry = custom_time_entry;
    gtk_box_pack_start(GTK_BOX(antiflapping_hbox), custom_time_entry, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(custom_time_entry), "changed", G_CALLBACK(on_custom_time_changed), plugin);
    g_signal_connect(G_OBJECT(custom_time_entry), "changed", G_CALLBACK(on_any_setting_changed), plugin);
    
    ms_label = gtk_label_new(_("ms"));
    gtk_box_pack_start(GTK_BOX(antiflapping_hbox), ms_label, FALSE, FALSE, 0);
    
    GtkWidget *error_label = gtk_label_new(_("Value must be between 100 and 5000 ms"));
    gtk_widget_set_halign(error_label, GTK_ALIGN_START);
    gtk_widget_set_visible(error_label, FALSE);
    gtk_widget_set_sensitive(error_label, FALSE);
    gtk_label_set_xalign(GTK_LABEL(error_label), 0.0);
    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css_provider, "label { color: #cc0000; }", -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(error_label),
                                   GTK_STYLE_PROVIDER(css_provider),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css_provider);
    state->custom_time_error_label = error_label;
    gtk_box_pack_start(GTK_BOX(antiflapping_vbox), error_label, FALSE, FALSE, 0);
    
    /* Buttons */
    button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(button_box, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(main_vbox), button_box, FALSE, FALSE, 0);
    apply_button = gtk_button_new_with_label(_("Apply"));
    gtk_widget_set_size_request(apply_button, 80, -1);
    g_signal_connect(G_OBJECT(apply_button), "clicked", G_CALLBACK(on_apply_clicked), plugin);
    gtk_box_pack_start(GTK_BOX(button_box), apply_button, FALSE, FALSE, 0);
    close_button = gtk_button_new_with_label(_("Close"));
    gtk_widget_set_size_request(close_button, 80, -1);
    g_signal_connect(G_OBJECT(close_button), "clicked", G_CALLBACK(on_close_button_clicked), dialog);
    gtk_box_pack_start(GTK_BOX(button_box), close_button, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(dialog), "destroy", G_CALLBACK(on_dialog_destroy), plugin);
    gtk_widget_show_all(dialog);
    
    settings_dialog_update_ui(plugin);
    
    settings_dialog_sync_from_asusd(plugin, FALSE);
    DEBUG_TRACE("=== create_settings_dialog: DIALOG SHOWN ===");
}

/* ========== Callbacks диалога ========== */

void on_close_button_clicked(G_GNUC_UNUSED GtkButton *button, GtkWidget *dialog) {
    DEBUG_TRACE("=== on_close_button_clicked: closing dialog ===");
    gtk_widget_destroy(dialog);
}

void on_dialog_destroy(GtkWidget *widget, AsusdBatteryPlugin *plugin) {
    if (!plugin || plugin->is_disposing) return;
    
    DEBUG_TRACE("on_dialog_destroy: settings dialog closed");
    
    if (plugin->settings_dialog_open) {
        plugin->settings_dialog_open = FALSE;
        plugin->saving_settings = FALSE;
    }
    
    if (plugin->dialog_state && plugin->dialog_state->dialog == widget) {
        g_free(plugin->dialog_state);
        plugin->dialog_state = NULL;
    }
}

void on_any_setting_changed(GtkWidget *widget, AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->dialog_state || plugin->is_disposing) return;
    if (plugin->dialog_state->syncing_ui) return;
    if (!plugin->settings_dialog_open) { 
        DEBUG_TRACE("on_any_setting_changed: dialog is closed"); 
        return; 
    }
    DEBUG_TRACE("on_any_setting_changed: setting changed");
    SettingsDialogState *state = plugin->dialog_state;
    
    if (widget == state->check_ac) { 
        state->dirty_ac_enabled = TRUE; 
        DEBUG_TRACE("  dirty_ac_enabled = TRUE"); 
    } else if (widget == state->check_battery) { 
        state->dirty_battery_enabled = TRUE; 
        DEBUG_TRACE("  dirty_battery_enabled = TRUE"); 
    } else if (widget == state->combo_ac) { 
        state->dirty_ac_profile = TRUE; 
        DEBUG_TRACE("  dirty_ac_profile = TRUE"); 
    } else if (widget == state->combo_battery) { 
        state->dirty_battery_profile = TRUE; 
        DEBUG_TRACE("  dirty_battery_profile = TRUE"); 
    } else if (widget == state->limit_check) { 
        state->dirty_limit = TRUE; 
        DEBUG_TRACE("  dirty_limit = TRUE"); 
    } else if (widget == state->antiflapping_check) {
        state->dirty_antiflapping = TRUE;
        DEBUG_TRACE("  dirty_antiflapping = TRUE");
    } else if (widget == state->custom_time_check) {
        state->dirty_custom_time = TRUE;
        DEBUG_TRACE("  dirty_custom_time = TRUE");
    } else if (widget == state->custom_time_entry) {
        state->dirty_timeout = TRUE;
        DEBUG_TRACE("  dirty_timeout = TRUE");
    } else if (widget == state->hide_icon_check || 
               widget == state->hide_text_check) {
        state->dirty_name = TRUE;
        DEBUG_TRACE("  dirty_name = TRUE (hide option changed)");
    } else if (widget == state->notifications_check) {
        state->dirty_name = TRUE;
        DEBUG_TRACE("  dirty_name = TRUE (hide notifications changed)");
    } else if (widget == state->fixed_width_check) {
        state->dirty_fixed_width = TRUE;
        DEBUG_TRACE("  dirty_fixed_width = TRUE");
    } else if (widget == state->fixed_width_entry) {
        state->dirty_fixed_width = TRUE;
        DEBUG_TRACE("  dirty_fixed_width = TRUE (entry changed)");
    } else if (widget == state->right_icon_check) {
        state->dirty_right_icon = TRUE;
        DEBUG_TRACE("  dirty_right_icon = TRUE");
    } else if (widget == state->align_combo) {
        state->dirty_align = TRUE;
        DEBUG_TRACE("  dirty_align = TRUE");
    } else if (GTK_IS_ENTRY(widget)) {
        GtkWidget *dialog = GTK_WIDGET(gtk_widget_get_toplevel(GTK_WIDGET(widget)));
        if (dialog && plugin->profiles) {
            for (guint i = 0; i < plugin->profiles->len; i++) {
                ProfileSettings *settings = g_ptr_array_index(plugin->profiles, i);
                gchar *key_name = g_strdup_printf("entry_name_%d", settings->enum_value);
                gchar *key_icon = g_strdup_printf("entry_icon_%d", settings->enum_value);
                
                GtkWidget *entry_name = g_object_get_data(G_OBJECT(dialog), key_name);
                GtkWidget *entry_icon = g_object_get_data(G_OBJECT(dialog), key_icon);
                
                if (widget == entry_name) {
                    state->dirty_name = TRUE;
                    DEBUG_TRACE("  dirty_name = TRUE (profile %d)", settings->enum_value);
                    g_free(key_name);
                    g_free(key_icon);
                    return;
                }
                if (widget == entry_icon) {
                    state->dirty_icon = TRUE;
                    DEBUG_TRACE("  dirty_icon = TRUE (profile %d)", settings->enum_value);
                    g_free(key_name);
                    g_free(key_icon);
                    return;
                }
                g_free(key_name);
                g_free(key_icon);
            }
        }
    }
}

void on_hide_toggle(GtkToggleButton *toggle_button, AsusdBatteryPlugin *plugin) {
    if (!plugin || plugin->is_disposing) return;
    if (plugin->dialog_state->syncing_ui) return;
    
    GtkWidget *dialog = GTK_WIDGET(gtk_widget_get_toplevel(GTK_WIDGET(toggle_button)));
    if (!dialog) return;
    GtkWidget *hide_icon_check = g_object_get_data(G_OBJECT(dialog), "hide_icon_check");
    GtkWidget *hide_text_check = g_object_get_data(G_OBJECT(dialog), "hide_text_check");
    gboolean icon_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(hide_icon_check));
    gboolean text_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(hide_text_check));
    
    if (icon_active && text_active) {
        if (toggle_button == GTK_TOGGLE_BUTTON(hide_icon_check))
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hide_text_check), FALSE);
        else
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hide_icon_check), FALSE);
        return;
    }
    
    if (plugin->dialog_state) {
        plugin->dialog_state->dirty_name = TRUE;
        DEBUG_TRACE("on_hide_toggle: dirty_name = TRUE (hide options changed)");
    }
}

void on_fixed_width_toggled(GtkToggleButton *toggle_button, AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->dialog_state || plugin->is_disposing) return;
    if (plugin->dialog_state->syncing_ui) return;
    
    gboolean active = gtk_toggle_button_get_active(toggle_button);
    
    if (plugin->dialog_state->fixed_width_entry) {
        gtk_widget_set_sensitive(plugin->dialog_state->fixed_width_entry, active);
    }
}

void on_notifications_toggled(GtkToggleButton *toggle_button, AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->dialog_state || plugin->is_disposing) return;
    if (plugin->dialog_state->syncing_ui) return;
    
    gboolean hidden = gtk_toggle_button_get_active(toggle_button);
    
    if (plugin->dialog_state->antiflapping_check) {
        gtk_widget_set_sensitive(plugin->dialog_state->antiflapping_check, !hidden);
    }
    if (plugin->dialog_state->custom_time_check) {
        gtk_widget_set_sensitive(plugin->dialog_state->custom_time_check, !hidden && 
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->antiflapping_check)));
    }
    if (plugin->dialog_state->custom_time_entry) {
        gtk_widget_set_sensitive(plugin->dialog_state->custom_time_entry, !hidden && 
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->custom_time_check)) &&
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->antiflapping_check)));
    }
}

void on_auto_switch_toggled(G_GNUC_UNUSED GtkToggleButton *toggle_button, GtkWidget *dialog) {
    GtkWidget *check_ac = g_object_get_data(G_OBJECT(dialog), "check_ac");
    GtkWidget *check_battery = g_object_get_data(G_OBJECT(dialog), "check_battery");
    GtkWidget *combo_ac = g_object_get_data(G_OBJECT(dialog), "combo_ac");
    GtkWidget *combo_battery = g_object_get_data(G_OBJECT(dialog), "combo_battery");
    if (check_ac && combo_ac) {
        gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check_ac));
        gtk_widget_set_sensitive(combo_ac, active);
    }
    if (check_battery && combo_battery) {
        gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check_battery));
        gtk_widget_set_sensitive(combo_battery, active);
    }
}

void on_antiflapping_toggled(GtkToggleButton *toggle_button, AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->dialog_state || plugin->is_disposing) return;
    if (plugin->dialog_state->syncing_ui) return;
    
    gboolean active = gtk_toggle_button_get_active(toggle_button);
    gboolean notif_hidden = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->notifications_check));
    
    if (plugin->dialog_state->custom_time_check) {
        gtk_widget_set_sensitive(plugin->dialog_state->custom_time_check, active && !notif_hidden);
        if (!active) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->custom_time_check), FALSE);
            if (plugin->dialog_state->custom_time_entry) {
                gtk_widget_set_sensitive(plugin->dialog_state->custom_time_entry, FALSE);
            }
        } else {
            gboolean custom_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->custom_time_check));
            if (plugin->dialog_state->custom_time_entry) {
                gtk_widget_set_sensitive(plugin->dialog_state->custom_time_entry, custom_active && !notif_hidden);
            }
        }
    }
}

void on_custom_time_toggled(GtkToggleButton *toggle_button, AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->dialog_state || plugin->is_disposing) return;
    if (plugin->dialog_state->syncing_ui) return;
    
    gboolean active = gtk_toggle_button_get_active(toggle_button);
    gboolean notif_hidden = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->notifications_check));
    gboolean antiflapping_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->antiflapping_check));
    
    if (plugin->dialog_state->custom_time_entry) {
        gtk_widget_set_sensitive(plugin->dialog_state->custom_time_entry, active && antiflapping_active && !notif_hidden);
    }
}

void on_custom_time_changed(GtkWidget *widget, AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->dialog_state) return;
    if (plugin->dialog_state->syncing_ui) return;
    
    SettingsDialogState *state = plugin->dialog_state;
    
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->custom_time_check))) {
        gtk_widget_set_visible(GTK_WIDGET(state->custom_time_error_label), FALSE);
        return;
    }
    
    if (!gtk_widget_get_sensitive(state->custom_time_entry)) {
        gtk_widget_set_visible(GTK_WIDGET(state->custom_time_error_label), FALSE);
        return;
    }
    
    guint dummy;
    validate_custom_time(GTK_ENTRY(widget), GTK_LABEL(state->custom_time_error_label), &dummy);
}

void on_one_shot_clicked(G_GNUC_UNUSED GtkButton *button, GtkWidget *dialog) {
    GtkWidget *message_dialog;
    gint response;
    AsusdBatteryPlugin *plugin = g_object_get_data(G_OBJECT(dialog), "plugin");
    if (!plugin || plugin->is_disposing) { DEBUG_WARN("on_one_shot_clicked: plugin invalid"); return; }
    
    GtkWidget *limit_check = g_object_get_data(G_OBJECT(dialog), "limit_check");
    gboolean limit_enabled_in_dialog = FALSE;
    if (limit_check) limit_enabled_in_dialog = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(limit_check));
    
    message_dialog = gtk_message_dialog_new(GTK_WINDOW(dialog), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
        _("Charge battery to 100%% once?\n\nThis will temporarily override the 80%% charge limit.\nThe limit will be restored after the next charge cycle."));
    gtk_window_set_icon_name(GTK_WINDOW(message_dialog), NULL);
    gtk_window_set_title(GTK_WINDOW(message_dialog), _("One-shot full charge"));
    gtk_dialog_set_default_response(GTK_DIALOG(message_dialog), GTK_RESPONSE_NO);
    response = gtk_dialog_run(GTK_DIALOG(message_dialog));
    gtk_widget_destroy(message_dialog);
    
    if (response == GTK_RESPONSE_YES) {
        if (!plugin->asusd_proxy) { DEBUG_WARN("ASUSD proxy not available"); return; }
        if (limit_enabled_in_dialog) {
            DEBUG_TRACE("on_one_shot_clicked: applying 80%% limit before one-shot");
            asusd_set_property_async(plugin, "ChargeControlEndThreshold", g_variant_new_byte(80), NULL, NULL);
        }
        asusd_call_async(plugin, "OneShotFullCharge", NULL, (GAsyncReadyCallback)on_one_shot_done, plugin);
    }
}

/* ========== Проверка custom time ========== */

static gboolean validate_custom_time(GtkEntry *entry, GtkLabel *error_label, guint *out_value) {
    const gchar *text = gtk_entry_get_text(entry);
    
    if (!text || strlen(text) == 0) {
        gtk_widget_set_visible(GTK_WIDGET(error_label), FALSE);
        return FALSE;
    }
    
    gchar *endptr;
    gulong val = strtoul(text, &endptr, 10);
    
    if (*endptr != '\0') {
        gtk_label_set_text(error_label, _("Please enter a number"));
        gtk_widget_set_visible(GTK_WIDGET(error_label), TRUE);
        return FALSE;
    }
    
    if (val < 100 || val > 5000) {
        gtk_label_set_text(error_label, _("Value must be between 100 and 5000 ms"));
        gtk_widget_set_visible(GTK_WIDGET(error_label), TRUE);
        return FALSE;
    }
    
    gtk_widget_set_visible(GTK_WIDGET(error_label), FALSE);
    if (out_value) {
        *out_value = (guint)val;
    }
    return TRUE;
}

/* ========== Применение настроек ========== */

void on_apply_clicked(GtkButton *button, AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->settings_dialog_open || plugin->is_disposing) return;
    
    DEBUG_TRACE("=== on_apply_clicked: Apply button clicked ===");
    
    GtkWidget *dialog = GTK_WIDGET(gtk_widget_get_toplevel(GTK_WIDGET(button)));
    if (!dialog) return;
    SettingsDialogState *state = plugin->dialog_state;
    if (!state) return;
    
    if (!state->dirty_ac_enabled && !state->dirty_battery_enabled && 
        !state->dirty_ac_profile && !state->dirty_battery_profile && 
        !state->dirty_limit && !state->dirty_name && !state->dirty_icon &&
        !state->dirty_antiflapping && !state->dirty_custom_time && !state->dirty_timeout &&
        !state->dirty_fixed_width && !state->dirty_right_icon && !state->dirty_align) {
        DEBUG_TRACE("  No changes detected in dialog, showing notification");
        if (!plugin->hide_notifications)
            send_notification(_("No changes"), _("Settings are already up to date"), FALSE, "emblem-default");
        return;
    }
    DEBUG_TRACE("  Changes detected in dialog");
    
    gboolean name_or_icon_changed = FALSE;
    
    if (plugin->profiles) {
        for (guint i = 0; i < plugin->profiles->len; i++) {
            ProfileSettings *settings = g_ptr_array_index(plugin->profiles, i);
            
            gchar *key_name = g_strdup_printf("entry_name_%d", settings->enum_value);
            GtkWidget *entry_name = g_object_get_data(G_OBJECT(dialog), key_name);
            if (entry_name) {
                const gchar *text = gtk_entry_get_text(GTK_ENTRY(entry_name));
                gchar *old_name = g_strdup(settings->name);
                
                if (text && strlen(text) > 0) {
                    g_free(settings->name);
                    settings->name = g_strdup(text);
                } else {
                    g_free(settings->name);
                    settings->name = NULL;
                }
                
                if (g_strcmp0(old_name, settings->name) != 0) {
                    name_or_icon_changed = TRUE;
                }
                g_free(old_name);
            }
            g_free(key_name);
            
            gchar *key_icon = g_strdup_printf("entry_icon_%d", settings->enum_value);
            GtkWidget *entry_icon = g_object_get_data(G_OBJECT(dialog), key_icon);
            if (entry_icon) {
                const gchar *text = gtk_entry_get_text(GTK_ENTRY(entry_icon));
                gchar *old_icon = g_strdup(settings->icon);
                
                const char *default_icon = NULL;
                if (g_strcmp0(settings->default_name, "performance") == 0) {
                    default_icon = "battery-full-symbolic";
                } else if (g_strcmp0(settings->default_name, "balanced") == 0) {
                    default_icon = "battery-good-symbolic";
                } else if (g_strcmp0(settings->default_name, "quiet") == 0) {
                    default_icon = "battery-low-symbolic";
                }
                
                if (text && strlen(text) > 0 && g_strcmp0(text, default_icon) != 0) {
                    g_free(settings->icon);
                    settings->icon = g_strdup(text);
                } else {
                    g_free(settings->icon);
                    settings->icon = NULL;
                }
                
                if (g_strcmp0(old_icon, settings->icon) != 0) {
                    name_or_icon_changed = TRUE;
                }
                g_free(old_icon);
            }
            g_free(key_icon);
        }
    }
    
    gboolean new_hide_icon = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->hide_icon_check));
    gboolean new_hide_text = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->hide_text_check));
    gboolean new_hide_notifications = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->notifications_check));
    
    gboolean hide_changed = FALSE;
    if (new_hide_icon != plugin->hide_icon) { 
        plugin->hide_icon = new_hide_icon; 
        hide_changed = TRUE; 
        DEBUG_DEBUG("hide_icon changed to: %d", new_hide_icon);
    }
    if (new_hide_text != plugin->hide_text) { 
        plugin->hide_text = new_hide_text; 
        hide_changed = TRUE;
        DEBUG_DEBUG("hide_text changed to: %d", new_hide_text);
    }
    if (new_hide_notifications != plugin->hide_notifications) { 
        plugin->hide_notifications = new_hide_notifications; 
        hide_changed = TRUE;
        DEBUG_DEBUG("hide_notifications changed to: %d", new_hide_notifications);
    }
    
    if (hide_changed) {
        if (plugin->hide_icon) {
            gtk_widget_hide(plugin->image);
        } else {
            gtk_widget_show(plugin->image);
        }
        if (plugin->hide_text) {
            gtk_widget_hide(plugin->label);
        } else {
            gtk_widget_show(plugin->label);
        }
    }
    
    gboolean new_fixed_width_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->fixed_width_check));
    guint new_fixed_width_value = plugin->fixed_width_value;
    const gchar *fw_text = gtk_entry_get_text(GTK_ENTRY(state->fixed_width_entry));
    if (fw_text && strlen(fw_text) > 0) {
        guint val = atoi(fw_text);
        if (val > 0) {
            new_fixed_width_value = val;
        }
    }
    gboolean new_right_icon = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->right_icon_check));
    gint new_align = gtk_combo_box_get_active(GTK_COMBO_BOX(state->align_combo));
    if (new_align < 0) new_align = 0;
    
    gboolean display_changed = FALSE;
    if (new_fixed_width_enabled != plugin->fixed_width_enabled) {
        plugin->fixed_width_enabled = new_fixed_width_enabled;
        display_changed = TRUE;
        DEBUG_DEBUG("fixed_width_enabled changed to: %d", new_fixed_width_enabled);
    }
    if (new_fixed_width_value != plugin->fixed_width_value) {
        plugin->fixed_width_value = new_fixed_width_value;
        display_changed = TRUE;
        DEBUG_DEBUG("fixed_width_value changed to: %d", new_fixed_width_value);
    }
    if (new_right_icon != plugin->right_icon) {
        plugin->right_icon = new_right_icon;
        display_changed = TRUE;
        DEBUG_DEBUG("right_icon changed to: %d", new_right_icon);
    }
    if (new_align != plugin->align) {
        plugin->align = new_align;
        display_changed = TRUE;
        DEBUG_DEBUG("align changed to: %d", new_align);
    }
    
    if (display_changed) {
        update_profile_display(plugin, FALSE);
    }
    
    gboolean new_antiflapping = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->antiflapping_check));
    gboolean new_custom_time = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->custom_time_check));
    guint new_timeout = DEFAULT_TIMEOUT_MS;
    
    if (new_custom_time) {
        if (state->custom_time_entry && gtk_widget_get_sensitive(state->custom_time_entry)) {
            guint validated_timeout;
            if (validate_custom_time(GTK_ENTRY(state->custom_time_entry), 
                                     GTK_LABEL(state->custom_time_error_label), 
                                     &validated_timeout)) {
                new_timeout = validated_timeout;
            } else {
                DEBUG_WARN("  Invalid custom timeout value, not applying");
                if (!plugin->hide_notifications) {
                    send_notification(_("Invalid value"), 
                                     _("Timeout must be between 100 and 5000 ms"), 
                                     TRUE, "dialog-error");
                }
                return;
            }
        }
    }
    
    gboolean antiflapping_changed = FALSE;
    if (new_antiflapping != plugin->enable_antiflapping) {
        plugin->enable_antiflapping = new_antiflapping;
        antiflapping_changed = TRUE;
    }
    if (new_custom_time != plugin->custom_time_enabled) {
        plugin->custom_time_enabled = new_custom_time;
        antiflapping_changed = TRUE;
    }
    if (new_timeout != plugin->custom_timeout_ms) {
        plugin->custom_timeout_ms = new_timeout;
        antiflapping_changed = TRUE;
        DEBUG_DEBUG("custom_timeout_ms changed to: %d (custom_time=%d)", new_timeout, new_custom_time);
    }
    
    if (name_or_icon_changed || hide_changed || display_changed || antiflapping_changed) {
        save_settings(plugin);
        DEBUG_TRACE("  UI changes saved (name/icon/hide/display options/anti-flapping)");
    }
    
    if (plugin->no_battery) {
        DEBUG_TRACE("  no_battery enabled, skipping D-Bus settings");
        settings_dialog_reset_dirty(plugin);
        plugin->saving_settings = FALSE;
        if (!plugin->hide_notifications && (name_or_icon_changed || hide_changed || display_changed || antiflapping_changed)) {
            send_notification(_("Settings applied"), _("UI settings updated"), FALSE, "emblem-system");
        }
        return;
    }
    
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) {
        DEBUG_TRACE("  ASUSD not available, cannot apply D-Bus settings");
        if (!plugin->hide_notifications)
            send_notification(_("Error"), _("ASUSD is not available. Cannot apply settings."), TRUE, "emblem-readonly");
        settings_dialog_reset_dirty(plugin);
        plugin->saving_settings = FALSE;
        return;
    }
    
    gboolean new_ac_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->check_ac));
    gboolean new_battery_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->check_battery));
    gboolean new_limit_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->limit_check));
    
    gchar *new_ac_profile = NULL;
    gchar *new_battery_profile = NULL;
    GtkTreeIter iter;
    if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(state->combo_ac), &iter)) {
        GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(state->combo_ac));
        gtk_tree_model_get(model, &iter, 0, &new_ac_profile, -1);
    }
    if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(state->combo_battery), &iter)) {
        GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(state->combo_battery));
        gtk_tree_model_get(model, &iter, 0, &new_battery_profile, -1);
    }
    
    gboolean dbus_changed = FALSE;
    if (state->dirty_ac_enabled && new_ac_enabled != plugin->auto_switch_ac_enabled) dbus_changed = TRUE;
    if (state->dirty_battery_enabled && new_battery_enabled != plugin->auto_switch_battery_enabled) dbus_changed = TRUE;
    if (state->dirty_ac_profile && new_ac_profile && g_strcmp0(new_ac_profile, plugin->auto_switch_ac_profile) != 0) dbus_changed = TRUE;
    if (state->dirty_battery_profile && new_battery_profile && g_strcmp0(new_battery_profile, plugin->auto_switch_battery_profile) != 0) dbus_changed = TRUE;
    if (state->dirty_limit && new_limit_enabled != plugin->battery_limit_enabled) dbus_changed = TRUE;
    
    if (!dbus_changed) {
        DEBUG_TRACE("  No D-Bus changes detected");
        if (!name_or_icon_changed && !hide_changed && !display_changed && !antiflapping_changed) {
            if (!plugin->hide_notifications)
                send_notification(_("No changes"), _("Settings are already up to date"), FALSE, "emblem-default");
        } else {
            if (!plugin->hide_notifications)
                send_notification(_("Settings applied"), _("UI settings updated"), FALSE, "emblem-system");
        }
        settings_dialog_reset_dirty(plugin);
        plugin->saving_settings = FALSE;
        g_free(new_ac_profile);
        g_free(new_battery_profile);
        return;
    }
    
    plugin->saving_settings = TRUE;
    
    SettingsApplyContext *ctx = g_new0(SettingsApplyContext, 1);
    g_weak_ref_init(&ctx->plugin_ref, G_OBJECT(plugin));
    ctx->new_ac_enabled = new_ac_enabled;
    ctx->new_battery_enabled = new_battery_enabled;
    ctx->new_ac_profile = g_strdup(new_ac_profile);
    ctx->new_battery_profile = g_strdup(new_battery_profile);
    ctx->new_limit_enabled = new_limit_enabled;
    ctx->new_limit = new_limit_enabled ? 80 : 100;
    ctx->current_step = 0;
    ctx->total_steps = 0;
    ctx->has_errors = FALSE;
    ctx->error_messages = NULL;
    ctx->error_count = 0;
    
    if (state->dirty_ac_enabled && new_ac_enabled != plugin->auto_switch_ac_enabled) ctx->total_steps++;
    if (state->dirty_battery_enabled && new_battery_enabled != plugin->auto_switch_battery_enabled) ctx->total_steps++;
    if (state->dirty_ac_profile && new_ac_profile && g_strcmp0(new_ac_profile, plugin->auto_switch_ac_profile) != 0) ctx->total_steps++;
    if (state->dirty_battery_profile && new_battery_profile && g_strcmp0(new_battery_profile, plugin->auto_switch_battery_profile) != 0) ctx->total_steps++;
    if (state->dirty_limit && ctx->new_limit != plugin->current_battery_limit) ctx->total_steps++;
    
    g_free(new_ac_profile);
    g_free(new_battery_profile);
    
    if (ctx->total_steps == 0) {
        DEBUG_TRACE("  No D-Bus steps after filtering");
        g_free(ctx);
        settings_dialog_reset_dirty(plugin);
        plugin->saving_settings = FALSE;
        return;
    }
    
    DEBUG_TRACE("  Starting apply with %d D-Bus steps", ctx->total_steps);
    apply_next_setting(ctx);
    
    update_profile_display(plugin, FALSE);
}

void apply_next_setting(SettingsApplyContext *ctx) {
    if (!ctx) return;
    
    AsusdBatteryPlugin *plugin = g_weak_ref_get(&ctx->plugin_ref);
    if (!plugin || plugin->is_disposing) {
        DEBUG_TRACE("apply_next_setting: plugin destroyed or disposing, aborting");
        if (plugin) g_object_unref(plugin);
        g_free(ctx->new_ac_profile); g_free(ctx->new_battery_profile);
        if (ctx->error_messages) g_strfreev(ctx->error_messages);
        g_free(ctx);
        return;
    }
    
    if (ctx->current_step >= ctx->total_steps || ctx->has_errors) {
        g_object_unref(plugin);
        on_settings_apply_complete(ctx);
        return;
    }
    DEBUG_TRACE("  Applying step %d/%d", ctx->current_step + 1, ctx->total_steps);
    
    int step = ctx->current_step;
    int applied = 0;
    
    if (step == applied && ctx->new_ac_enabled != plugin->auto_switch_ac_enabled) {
        asusd_set_property_async(plugin, "ChangePlatformProfileOnAc",
                                g_variant_new_boolean(ctx->new_ac_enabled),
                                (GAsyncReadyCallback)on_settings_apply_step_done, ctx);
        g_object_unref(plugin);
        return;
    }
    if (ctx->new_ac_enabled != plugin->auto_switch_ac_enabled) applied++;
    
    if (step == applied && ctx->new_battery_enabled != plugin->auto_switch_battery_enabled) {
        asusd_set_property_async(plugin, "ChangePlatformProfileOnBattery",
                                g_variant_new_boolean(ctx->new_battery_enabled),
                                (GAsyncReadyCallback)on_settings_apply_step_done, ctx);
        g_object_unref(plugin);
        return;
    }
    if (ctx->new_battery_enabled != plugin->auto_switch_battery_enabled) applied++;
    
    if (step == applied && ctx->new_ac_profile &&
        g_strcmp0(ctx->new_ac_profile, plugin->auto_switch_ac_profile) != 0) {
        guint32 enum_val = 999;
        if (!profile_enum_from_name(plugin, ctx->new_ac_profile, &enum_val) || enum_val == 999) {
            DEBUG_WARN("  Failed to find enum for profile: %s", ctx->new_ac_profile);
            ctx->has_errors = TRUE; ctx->error_count++; ctx->current_step++;
            g_object_unref(plugin);
            apply_next_setting(ctx);
            return;
        }
        asusd_set_property_async(plugin, "PlatformProfileOnAc",
                                g_variant_new_uint32(enum_val),
                                (GAsyncReadyCallback)on_settings_apply_step_done, ctx);
        g_object_unref(plugin);
        return;
    }
    if (ctx->new_ac_profile && g_strcmp0(ctx->new_ac_profile, plugin->auto_switch_ac_profile) != 0) applied++;
    
    if (step == applied && ctx->new_battery_profile &&
        g_strcmp0(ctx->new_battery_profile, plugin->auto_switch_battery_profile) != 0) {
        guint32 enum_val = 999;
        if (!profile_enum_from_name(plugin, ctx->new_battery_profile, &enum_val) || enum_val == 999) {
            DEBUG_WARN("  Failed to find enum for profile: %s", ctx->new_battery_profile);
            ctx->has_errors = TRUE; ctx->error_count++; ctx->current_step++;
            g_object_unref(plugin);
            apply_next_setting(ctx);
            return;
        }
        asusd_set_property_async(plugin, "PlatformProfileOnBattery",
                                g_variant_new_uint32(enum_val),
                                (GAsyncReadyCallback)on_settings_apply_step_done, ctx);
        g_object_unref(plugin);
        return;
    }
    if (ctx->new_battery_profile && g_strcmp0(ctx->new_battery_profile, plugin->auto_switch_battery_profile) != 0) applied++;
    
    if (step == applied && ctx->new_limit != plugin->current_battery_limit) {
        asusd_set_property_async(plugin, "ChargeControlEndThreshold",
                                g_variant_new_byte(ctx->new_limit),
                                (GAsyncReadyCallback)on_settings_apply_step_done, ctx);
        g_object_unref(plugin);
        return;
    }
    
    g_object_unref(plugin);
    on_settings_apply_complete(ctx);
}

void on_settings_apply_step_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    SettingsApplyContext *ctx = (SettingsApplyContext*)user_data;
    if (!ctx) return;
    
    AsusdBatteryPlugin *plugin = g_weak_ref_get(&ctx->plugin_ref);
    if (!plugin || plugin->is_disposing) {
        DEBUG_TRACE("on_settings_apply_step_done: plugin destroyed or disposing");
        if (plugin) g_object_unref(plugin);
        g_free(ctx->new_ac_profile); g_free(ctx->new_battery_profile);
        if (ctx->error_messages) g_strfreev(ctx->error_messages);
        g_free(ctx);
        return;
    }
    
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error) {
        DEBUG_WARN("  Failed to apply setting: %s", error->message);
        ctx->has_errors = TRUE; ctx->error_count++;
        if (ctx->error_messages) ctx->error_messages = g_realloc(ctx->error_messages, (ctx->error_count + 1) * sizeof(gchar*));
        else ctx->error_messages = g_new0(gchar*, 2);
        ctx->error_messages[ctx->error_count - 1] = g_strdup(error->message);
        ctx->error_messages[ctx->error_count] = NULL;
        g_error_free(error);
    } else {
        if (result) g_variant_unref(result);
    }
    ctx->current_step++;
    g_object_unref(plugin);
    apply_next_setting(ctx);
}

void on_settings_apply_complete(SettingsApplyContext *ctx) {
    if (!ctx) return;
    
    AsusdBatteryPlugin *plugin = g_weak_ref_get(&ctx->plugin_ref);
    
    if (plugin && !plugin->is_disposing) {
        plugin->saving_settings = FALSE;
        
        if (ctx->has_errors) {
            DEBUG_WARN("  One or more settings failed to apply");
            if (!plugin->hide_notifications) {
                gchar *error_msg = NULL;
                if (ctx->error_count > 0 && ctx->error_messages)
                    error_msg = g_strjoinv("\n", ctx->error_messages);
                send_notification(_("Error applying settings"),
                                 error_msg ? error_msg : _("Some settings could not be applied."),
                                 TRUE, "emblem-readonly");
                g_free(error_msg);
            }
        } else {
            plugin->auto_switch_ac_enabled = ctx->new_ac_enabled;
            plugin->auto_switch_battery_enabled = ctx->new_battery_enabled;
            if (ctx->new_ac_profile) { g_free(plugin->auto_switch_ac_profile); plugin->auto_switch_ac_profile = g_strdup(ctx->new_ac_profile); }
            if (ctx->new_battery_profile) { g_free(plugin->auto_switch_battery_profile); plugin->auto_switch_battery_profile = g_strdup(ctx->new_battery_profile); }
            plugin->battery_limit_enabled = ctx->new_limit_enabled;
            plugin->current_battery_limit = ctx->new_limit;
            settings_dialog_reset_dirty(plugin);
            save_settings(plugin);
            DEBUG_TRACE("  Changes applied successfully");
            if (!plugin->hide_notifications)
                send_notification(_("Settings applied"), _("Power profile settings have been updated"), FALSE, "emblem-system");
        }
        
        if (plugin->dialog_state && plugin->dialog_state->dialog) {
            settings_dialog_update_ui(plugin);
        }
    } else {
        DEBUG_TRACE("on_settings_apply_complete: plugin destroyed or disposing, skipping UI updates");
    }
    
    if (plugin) {
        g_object_unref(plugin);
    }
    g_weak_ref_clear(&ctx->plugin_ref);
    g_free(ctx->new_ac_profile); g_free(ctx->new_battery_profile);
    if (ctx->error_messages) g_strfreev(ctx->error_messages);
    g_free(ctx);
}
