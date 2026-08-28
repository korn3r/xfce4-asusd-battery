#include "settings-dialog.h"
#include "utils.h"
#include "profile-manager.h"
#include "config.h"
#include "plugin.h"
#include "asusd-client.h"
#include "debug.h"
#include <libxfce4util/libxfce4util.h>

/* ========== Функции диалога настроек ========== */

void settings_dialog_reset_dirty(AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->dialog_state) return;
    plugin->dialog_state->dirty_ac_enabled = FALSE;
    plugin->dialog_state->dirty_battery_enabled = FALSE;
    plugin->dialog_state->dirty_ac_profile = FALSE;
    plugin->dialog_state->dirty_battery_profile = FALSE;
    plugin->dialog_state->dirty_limit = FALSE;
}

void settings_dialog_update_ui(AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->dialog_state || plugin->is_disposing) return;
    SettingsDialogState *state = plugin->dialog_state;
    state->syncing_ui = TRUE;
    
    if (!plugin->profiles || plugin->profiles->len == 0) {
        DEBUG_TRACE("settings_dialog_update_ui: No profiles loaded, creating fallback");
        create_fallback_profiles(plugin);
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
    if (state->notifications_check)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->notifications_check), plugin->hide_notifications);
    
    if (state->combo_ac && !state->dirty_ac_profile && plugin->auto_switch_ac_profile) {
        GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(state->combo_ac));
        if (model) {
            GtkTreeIter iter;
            gboolean found = FALSE;
            gchar *profile_name = NULL;
            gint index = 0;
            if (gtk_tree_model_get_iter_first(model, &iter)) {
                do {
                    gtk_tree_model_get(model, &iter, 0, &profile_name, -1);
                    if (g_strcmp0(profile_name, plugin->auto_switch_ac_profile) == 0) { found = TRUE; break; }
                    g_free(profile_name);
                    index++;
                } while (gtk_tree_model_iter_next(model, &iter));
                g_free(profile_name);
            }
            if (found) gtk_combo_box_set_active(GTK_COMBO_BOX(state->combo_ac), index);
        }
    }
    if (state->combo_battery && !state->dirty_battery_profile && plugin->auto_switch_battery_profile) {
        GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(state->combo_battery));
        if (model) {
            GtkTreeIter iter;
            gboolean found = FALSE;
            gchar *profile_name = NULL;
            gint index = 0;
            if (gtk_tree_model_get_iter_first(model, &iter)) {
                do {
                    gtk_tree_model_get(model, &iter, 0, &profile_name, -1);
                    if (g_strcmp0(profile_name, plugin->auto_switch_battery_profile) == 0) { found = TRUE; break; }
                    g_free(profile_name);
                    index++;
                } while (gtk_tree_model_iter_next(model, &iter));
                g_free(profile_name);
            }
            if (found) gtk_combo_box_set_active(GTK_COMBO_BOX(state->combo_battery), index);
        }
    }
    state->syncing_ui = FALSE;
}

void settings_dialog_sync_from_asusd(AsusdBatteryPlugin *plugin, gboolean keep_dirty) {
    if (!plugin || !plugin->dialog_state || plugin->is_disposing) return;
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) {
        DEBUG_TRACE("settings_dialog_sync_from_asusd: ASUSD not available, using cached values");
        settings_dialog_update_ui(plugin);
        return;
    }
    
    guint dialog_id = plugin->dialog_state->dialog_id;
    plugin->dialog_state->syncing_ui = TRUE;
    DEBUG_TRACE("settings_dialog_sync_from_asusd: Loading settings from ASUSD");
    
    AsyncCallContext *ctx = async_call_context_new(plugin, NULL, NULL,
                                                   (GAsyncReadyCallback)on_dialog_property_loaded, NULL, NULL);
    ctx->dialog_id = dialog_id;
    ctx->is_dialog_callback = TRUE;
    ctx->user_data = GINT_TO_POINTER(0);
    
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
    int step = GPOINTER_TO_INT(ctx->user_data);
    
    if (!plugin->dialog_state) {
        DEBUG_TRACE("on_dialog_property_loaded: dialog_state is NULL, discarding");
        g_object_unref(plugin);
        async_call_context_free(ctx);
        return;
    }
    
    if (!is_dialog_valid(plugin, dialog_id)) {
        DEBUG_TRACE("on_dialog_property_loaded: dialog changed or destroyed (id %u)", dialog_id);
        g_object_unref(plugin);
        async_call_context_free(ctx);
        return;
    }
    
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    
    switch (step) {
        case 0: {
            if (error || !result) {
                if (error) DEBUG_TRACE("Failed to get ChangePlatformProfileOnAc: %s", error->message);
                else DEBUG_TRACE("No result for ChangePlatformProfileOnAc");
                if (error) g_error_free(error);
                plugin->auto_switch_ac_enabled = FALSE;
                if (is_dialog_valid(plugin, dialog_id) && plugin->dialog_state->check_ac && !plugin->dialog_state->dirty_ac_enabled)
                    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->check_ac), FALSE);
            } else {
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
            
            ctx->user_data = GINT_TO_POINTER(1);
            asusd_get_property_async(plugin, "ChangePlatformProfileOnBattery",
                                    (GAsyncReadyCallback)on_dialog_property_loaded, ctx);
            break;
        }
        case 1: {
            if (error || !result) {
                if (error) DEBUG_TRACE("Failed to get ChangePlatformProfileOnBattery: %s", error->message);
                else DEBUG_TRACE("No result for ChangePlatformProfileOnBattery");
                if (error) g_error_free(error);
                plugin->auto_switch_battery_enabled = FALSE;
                if (is_dialog_valid(plugin, dialog_id) && plugin->dialog_state->check_battery && !plugin->dialog_state->dirty_battery_enabled)
                    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->check_battery), FALSE);
            } else {
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
            
            ctx->user_data = GINT_TO_POINTER(2);
            asusd_get_property_async(plugin, "PlatformProfileOnAc",
                                    (GAsyncReadyCallback)on_dialog_property_loaded, ctx);
            break;
        }
        case 2: {
            const char *default_profile = "balanced";
            if (plugin->profiles && plugin->profiles->len > 0) {
                ProfileSettings *settings = g_ptr_array_index(plugin->profiles, 0);
                if (settings->default_name) default_profile = settings->default_name;
            }
            
            if (error || !result) {
                if (error) DEBUG_TRACE("Failed to get PlatformProfileOnAc: %s", error->message);
                else DEBUG_TRACE("No result for PlatformProfileOnAc");
                if (error) g_error_free(error);
                g_free(plugin->auto_switch_ac_profile);
                plugin->auto_switch_ac_profile = g_strdup(default_profile);
            } else {
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
                            GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(plugin->dialog_state->combo_ac));
                            if (model) {
                                GtkTreeIter iter;
                                gboolean found = FALSE;
                                gchar *profile_name = NULL;
                                gint index = 0;
                                if (gtk_tree_model_get_iter_first(model, &iter)) {
                                    do {
                                        gtk_tree_model_get(model, &iter, 0, &profile_name, -1);
                                        if (g_strcmp0(profile_name, name) == 0) { found = TRUE; break; }
                                        g_free(profile_name);
                                        index++;
                                    } while (gtk_tree_model_iter_next(model, &iter));
                                    g_free(profile_name);
                                }
                                if (found) gtk_combo_box_set_active(GTK_COMBO_BOX(plugin->dialog_state->combo_ac), index);
                            }
                        }
                    }
                    g_variant_unref(value);
                }
                g_variant_unref(result);
            }
            
            if (is_dialog_valid(plugin, dialog_id))
                plugin->dialog_state->syncing_ui = TRUE;
            
            ctx->user_data = GINT_TO_POINTER(3);
            asusd_get_property_async(plugin, "PlatformProfileOnBattery",
                                    (GAsyncReadyCallback)on_dialog_property_loaded, ctx);
            break;
        }
        case 3: {
            const char *default_profile = "balanced";
            if (plugin->profiles && plugin->profiles->len > 0) {
                ProfileSettings *settings = g_ptr_array_index(plugin->profiles, 0);
                if (settings->default_name) default_profile = settings->default_name;
            }
            
            if (error || !result) {
                if (error) DEBUG_TRACE("Failed to get PlatformProfileOnBattery: %s", error->message);
                else DEBUG_TRACE("No result for PlatformProfileOnBattery");
                if (error) g_error_free(error);
                g_free(plugin->auto_switch_battery_profile);
                plugin->auto_switch_battery_profile = g_strdup(default_profile);
            } else {
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
                            GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(plugin->dialog_state->combo_battery));
                            if (model) {
                                GtkTreeIter iter;
                                gboolean found = FALSE;
                                gchar *profile_name = NULL;
                                gint index = 0;
                                if (gtk_tree_model_get_iter_first(model, &iter)) {
                                    do {
                                        gtk_tree_model_get(model, &iter, 0, &profile_name, -1);
                                        if (g_strcmp0(profile_name, name) == 0) { found = TRUE; break; }
                                        g_free(profile_name);
                                        index++;
                                    } while (gtk_tree_model_iter_next(model, &iter));
                                    g_free(profile_name);
                                }
                                if (found) gtk_combo_box_set_active(GTK_COMBO_BOX(plugin->dialog_state->combo_battery), index);
                            }
                        }
                    }
                    g_variant_unref(value);
                }
                g_variant_unref(result);
            }
            
            if (is_dialog_valid(plugin, dialog_id))
                plugin->dialog_state->syncing_ui = TRUE;
            
            ctx->user_data = GINT_TO_POINTER(4);
            asusd_get_property_async(plugin, "ChargeControlEndThreshold",
                                    (GAsyncReadyCallback)on_dialog_property_loaded, ctx);
            break;
        }
        case 4: {
            if (error || !result) {
                if (error) DEBUG_TRACE("Failed to get ChargeControlEndThreshold: %s", error->message);
                else DEBUG_TRACE("No result for ChargeControlEndThreshold");
                if (error) g_error_free(error);
                plugin->current_battery_limit = 100;
                plugin->battery_limit_enabled = FALSE;
                if (is_dialog_valid(plugin, dialog_id) && plugin->dialog_state->limit_check && !plugin->dialog_state->dirty_limit)
                    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->limit_check), FALSE);
            } else {
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
            
            DEBUG_TRACE("Dialog settings loaded completely");
            async_call_context_free(ctx);
            g_object_unref(plugin);
            return;
        }
        default:
            break;
    }
    
    g_object_unref(plugin);
}

/* ========== Создание диалога ========== */

void create_settings_dialog(AsusdBatteryPlugin *plugin) {
    if (!plugin || plugin->is_disposing) return;
    
    if (plugin->settings_dialog_open && plugin->dialog_state && plugin->dialog_state->dialog) {
        DEBUG_TRACE("create_settings_dialog: dialog already open");
        gtk_window_present(GTK_WINDOW(plugin->dialog_state->dialog));
        return;
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
    GtkWidget *one_shot_hbox;
    GtkWidget *one_shot_label;
    GtkWidget *one_shot_button;
    GtkWidget *button_box;
    GtkWidget *apply_button;
    GtkWidget *close_button;
    GtkWidget *options_frame;
    GtkWidget *options_vbox;
    GtkWidget *options_hbox;
    GtkWidget *hide_label;
    GtkWidget *separator;
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
            if (plugin->auto_switch_ac_profile && g_strcmp0(name, plugin->auto_switch_ac_profile) == 0) active_index = i;
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
            if (plugin->auto_switch_battery_profile && g_strcmp0(name, plugin->auto_switch_battery_profile) == 0) active_index = i;
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo_battery), active_index);
    }
    gtk_widget_set_sensitive(combo_battery, plugin->auto_switch_battery_enabled && plugin->asusd_state == ASUSD_STATE_AVAILABLE);
    state->combo_battery = combo_battery;
    g_object_set_data(G_OBJECT(dialog), "combo_battery", combo_battery);
    gtk_box_pack_start(GTK_BOX(hbox_battery), combo_battery, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(combo_battery), "changed", G_CALLBACK(on_any_setting_changed), plugin);
    g_signal_connect(G_OBJECT(check_battery), "toggled", G_CALLBACK(on_auto_switch_toggled), dialog);
    
    GtkWidget *limit_frame = gtk_frame_new(_("Battery charge limit"));
    gtk_box_pack_start(GTK_BOX(main_vbox), limit_frame, FALSE, FALSE, 0);
    GtkWidget *limit_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(limit_vbox), 5);
    gtk_container_add(GTK_CONTAINER(limit_frame), limit_vbox);
    limit_check_widget = gtk_check_button_new_with_label(_("Limit battery charge to 80%"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(limit_check_widget), plugin->battery_limit_enabled);
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) gtk_widget_set_sensitive(limit_check_widget, FALSE);
    state->limit_check = limit_check_widget;
    g_object_set_data(G_OBJECT(dialog), "limit_check", limit_check_widget);
    gtk_box_pack_start(GTK_BOX(limit_vbox), limit_check_widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(limit_check_widget), "toggled", G_CALLBACK(on_any_setting_changed), plugin);
    
    one_shot_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(limit_vbox), one_shot_hbox, FALSE, FALSE, 0);
    one_shot_label = gtk_label_new(_("Charge battery to 100% once:"));
    gtk_box_pack_start(GTK_BOX(one_shot_hbox), one_shot_label, FALSE, FALSE, 0);
    one_shot_button = gtk_button_new_with_label(_("Start"));
    g_signal_connect(G_OBJECT(one_shot_button), "clicked", G_CALLBACK(on_one_shot_clicked), dialog);
    gtk_box_pack_start(GTK_BOX(one_shot_hbox), one_shot_button, FALSE, FALSE, 0);
    
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
            if (settings->icon && strlen(settings->icon) > 0) gtk_entry_set_text(GTK_ENTRY(entry_icon), settings->icon);
            gtk_entry_set_placeholder_text(GTK_ENTRY(entry_icon), _("icon name"));
            gtk_widget_set_size_request(entry_icon, 120, -1);
            g_object_set_data(G_OBJECT(dialog), g_strdup_printf("entry_icon_%d", settings->enum_value), entry_icon);
            gtk_grid_attach(GTK_GRID(grid), entry_icon, 2, row, 1, 1);
            g_signal_connect(G_OBJECT(entry_icon), "changed", G_CALLBACK(on_any_setting_changed), plugin);
            row++;
        }
    }
    
    options_frame = gtk_frame_new(_("Display options"));
    gtk_box_pack_start(GTK_BOX(main_vbox), options_frame, FALSE, FALSE, 0);
    options_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(options_vbox), 5);
    gtk_container_add(GTK_CONTAINER(options_frame), options_vbox);
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
    hide_text_check_widget = gtk_check_button_new_with_label(_("Text"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hide_text_check_widget), plugin->hide_text);
    gtk_box_pack_start(GTK_BOX(options_hbox), hide_text_check_widget, FALSE, FALSE, 0);
    state->hide_text_check = hide_text_check_widget;
    g_object_set_data(G_OBJECT(dialog), "hide_text_check", hide_text_check_widget);
    g_signal_connect(G_OBJECT(hide_text_check_widget), "toggled", G_CALLBACK(on_hide_toggle), plugin);
    notifications_check_widget = gtk_check_button_new_with_label(_("Notifications"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(notifications_check_widget), plugin->hide_notifications);
    gtk_box_pack_start(GTK_BOX(options_hbox), notifications_check_widget, FALSE, FALSE, 0);
    state->notifications_check = notifications_check_widget;
    g_object_set_data(G_OBJECT(dialog), "notifications_check", notifications_check_widget);
    g_signal_connect(G_OBJECT(notifications_check_widget), "toggled", G_CALLBACK(on_any_setting_changed), plugin);
    
    separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(main_vbox), separator, FALSE, FALSE, 5);
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
    
    settings_dialog_sync_from_asusd(plugin, FALSE);
    DEBUG_TRACE("=== create_settings_dialog: DIALOG SHOWN ===");
}

/* ========== Callbacks диалога ========== */

void on_close_button_clicked(GtkButton *button, GtkWidget *dialog) {
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
    if (!plugin->settings_dialog_open) { DEBUG_TRACE("on_any_setting_changed: dialog is closed"); return; }
    DEBUG_TRACE("on_any_setting_changed: setting changed");
    SettingsDialogState *state = plugin->dialog_state;
    if (widget == state->check_ac) { state->dirty_ac_enabled = TRUE; DEBUG_TRACE("  dirty_ac_enabled = TRUE"); }
    else if (widget == state->check_battery) { state->dirty_battery_enabled = TRUE; DEBUG_TRACE("  dirty_battery_enabled = TRUE"); }
    else if (widget == state->combo_ac) { state->dirty_ac_profile = TRUE; DEBUG_TRACE("  dirty_ac_profile = TRUE"); }
    else if (widget == state->combo_battery) { state->dirty_battery_profile = TRUE; DEBUG_TRACE("  dirty_battery_profile = TRUE"); }
    else if (widget == state->limit_check) { state->dirty_limit = TRUE; DEBUG_TRACE("  dirty_limit = TRUE"); }
}

void on_hide_toggle(GtkToggleButton *toggle_button, AsusdBatteryPlugin *plugin) {
    if (!plugin || plugin->is_disposing) return;
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
    }
}

void on_auto_switch_toggled(GtkToggleButton *toggle_button, GtkWidget *dialog) {
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

void on_one_shot_clicked(GtkButton *button, GtkWidget *dialog) {
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

/* ========== Применение настроек ========== */

void on_apply_clicked(GtkButton *button, AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->settings_dialog_open || plugin->is_disposing) return;
    
    DEBUG_TRACE("=== on_apply_clicked: Apply button clicked ===");
    
    GtkWidget *dialog = GTK_WIDGET(gtk_widget_get_toplevel(GTK_WIDGET(button)));
    if (!dialog) return;
    SettingsDialogState *state = plugin->dialog_state;
    if (!state) return;
    
    gboolean new_hide_icon = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->hide_icon_check));
    gboolean new_hide_text = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->hide_text_check));
    gboolean new_hide_notifications = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->notifications_check));
    
    gboolean hide_changed = FALSE;
    if (new_hide_icon != plugin->hide_icon) { plugin->hide_icon = new_hide_icon; hide_changed = TRUE; }
    if (new_hide_text != plugin->hide_text) { plugin->hide_text = new_hide_text; hide_changed = TRUE; }
    if (new_hide_notifications != plugin->hide_notifications) { plugin->hide_notifications = new_hide_notifications; hide_changed = TRUE; }
    if (hide_changed) update_profile_display(plugin, FALSE);
    
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) {
        DEBUG_TRACE("  ASUSD not available, cannot apply settings");
        if (!plugin->hide_notifications)
            send_notification(_("Error"), _("ASUSD is not available. Cannot apply settings."), TRUE, "emblem-readonly");
        save_settings(plugin);
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
    
    plugin->saving_settings = TRUE;
    
    SettingsApplyContext *ctx = g_new0(SettingsApplyContext, 1);
    g_weak_ref_init(&ctx->plugin_ref, G_OBJECT(plugin));
    ctx->plugin = g_object_ref(plugin);
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
    
    if (new_ac_enabled != plugin->auto_switch_ac_enabled) ctx->total_steps++;
    if (new_battery_enabled != plugin->auto_switch_battery_enabled) ctx->total_steps++;
    if (new_ac_profile && g_strcmp0(new_ac_profile, plugin->auto_switch_ac_profile) != 0) ctx->total_steps++;
    if (new_battery_profile && g_strcmp0(new_battery_profile, plugin->auto_switch_battery_profile) != 0) ctx->total_steps++;
    if (ctx->new_limit != plugin->current_battery_limit) ctx->total_steps++;
    
    if (ctx->total_steps == 0) {
        DEBUG_TRACE("  No ASUSD changes detected");
        save_settings(plugin);
        settings_dialog_reset_dirty(plugin);
        if (!plugin->hide_notifications)
            send_notification(_("No changes"), _("Settings are already up to date"), FALSE, "emblem-default");
        g_weak_ref_clear(&ctx->plugin_ref);
        g_object_unref(ctx->plugin);
        g_free(ctx->new_ac_profile); g_free(ctx->new_battery_profile);
        g_free(ctx);
        plugin->saving_settings = FALSE;
        return;
    }
    
    DEBUG_TRACE("  Starting apply with %d steps", ctx->total_steps);
    apply_next_setting(ctx);
}

void apply_next_setting(SettingsApplyContext *ctx) {
    if (!ctx) return;
    
    AsusdBatteryPlugin *plugin = ctx->plugin;
    if (!plugin || plugin->is_disposing) {
        DEBUG_TRACE("apply_next_setting: plugin destroyed or disposing, aborting");
        g_free(ctx->new_ac_profile); g_free(ctx->new_battery_profile);
        if (ctx->error_messages) g_strfreev(ctx->error_messages);
        g_free(ctx);
        return;
    }
    
    if (ctx->current_step >= ctx->total_steps || ctx->has_errors) {
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
        return;
    }
    if (ctx->new_ac_enabled != plugin->auto_switch_ac_enabled) applied++;
    
    if (step == applied && ctx->new_battery_enabled != plugin->auto_switch_battery_enabled) {
        asusd_set_property_async(plugin, "ChangePlatformProfileOnBattery",
                                g_variant_new_boolean(ctx->new_battery_enabled),
                                (GAsyncReadyCallback)on_settings_apply_step_done, ctx);
        return;
    }
    if (ctx->new_battery_enabled != plugin->auto_switch_battery_enabled) applied++;
    
    if (step == applied && ctx->new_ac_profile &&
        g_strcmp0(ctx->new_ac_profile, plugin->auto_switch_ac_profile) != 0) {
        guint32 enum_val = 999;
        if (!profile_enum_from_name(plugin, ctx->new_ac_profile, &enum_val) || enum_val == 999) {
            DEBUG_WARN("  Failed to find enum for profile: %s", ctx->new_ac_profile);
            ctx->has_errors = TRUE; ctx->error_count++; ctx->current_step++;
            apply_next_setting(ctx);
            return;
        }
        asusd_set_property_async(plugin, "PlatformProfileOnAc",
                                g_variant_new_uint32(enum_val),
                                (GAsyncReadyCallback)on_settings_apply_step_done, ctx);
        return;
    }
    if (ctx->new_ac_profile && g_strcmp0(ctx->new_ac_profile, plugin->auto_switch_ac_profile) != 0) applied++;
    
    if (step == applied && ctx->new_battery_profile &&
        g_strcmp0(ctx->new_battery_profile, plugin->auto_switch_battery_profile) != 0) {
        guint32 enum_val = 999;
        if (!profile_enum_from_name(plugin, ctx->new_battery_profile, &enum_val) || enum_val == 999) {
            DEBUG_WARN("  Failed to find enum for profile: %s", ctx->new_battery_profile);
            ctx->has_errors = TRUE; ctx->error_count++; ctx->current_step++;
            apply_next_setting(ctx);
            return;
        }
        asusd_set_property_async(plugin, "PlatformProfileOnBattery",
                                g_variant_new_uint32(enum_val),
                                (GAsyncReadyCallback)on_settings_apply_step_done, ctx);
        return;
    }
    if (ctx->new_battery_profile && g_strcmp0(ctx->new_battery_profile, plugin->auto_switch_battery_profile) != 0) applied++;
    
    if (step == applied && ctx->new_limit != plugin->current_battery_limit) {
        asusd_set_property_async(plugin, "ChargeControlEndThreshold",
                                g_variant_new_byte(ctx->new_limit),
                                (GAsyncReadyCallback)on_settings_apply_step_done, ctx);
        return;
    }
    
    on_settings_apply_complete(ctx);
}

void on_settings_apply_step_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    SettingsApplyContext *ctx = (SettingsApplyContext*)user_data;
    if (!ctx) return;
    
    AsusdBatteryPlugin *plugin = ctx->plugin;
    if (!plugin || plugin->is_disposing) {
        DEBUG_TRACE("on_settings_apply_step_done: plugin destroyed or disposing");
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
    apply_next_setting(ctx);
}

void on_settings_apply_complete(SettingsApplyContext *ctx) {
    if (!ctx) return;
    
    AsusdBatteryPlugin *plugin = ctx->plugin;
    
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
