/* src/main.c */
#include "plugin.h"
#include "utils.h"
#include "profile-manager.h"
#include "asusd-client.h"
#include "settings-dialog.h"

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>
#include <libxfce4util/libxfce4util.h>
#include <xfconf/xfconf.h>
#include "debug.h"

/* ========== GObject определения ========== */

G_DEFINE_TYPE(AsusdBatteryPlugin, asusd_battery_plugin, G_TYPE_OBJECT)

static void asusd_battery_plugin_dispose(GObject *object) {
    AsusdBatteryPlugin *plugin = ASUSD_BATTERY_PLUGIN(object);
    
    if (plugin->is_disposing) return;
    plugin->is_disposing = TRUE;
    
    DEBUG_DEBUG("AsusdBatteryPlugin: dispose");
    
    asusd_cleanup(plugin);
    
    G_OBJECT_CLASS(asusd_battery_plugin_parent_class)->dispose(object);
}

static void asusd_battery_plugin_finalize(GObject *object) {
    AsusdBatteryPlugin *plugin = ASUSD_BATTERY_PLUGIN(object);
    
    DEBUG_DEBUG("AsusdBatteryPlugin: finalize");
    
    g_free(plugin->current_profile);
    g_free(plugin->auto_switch_ac_profile);
    g_free(plugin->auto_switch_battery_profile);
    g_free(plugin->last_displayed_profile);
    
    if (plugin->profiles) {
        g_ptr_array_free(plugin->profiles, TRUE);
        plugin->profiles = NULL;
    }
    if (plugin->profile_lookup) {
        g_hash_table_destroy(plugin->profile_lookup);
        plugin->profile_lookup = NULL;
    }
    if (plugin->dialog_state) {
        g_free(plugin->dialog_state);
        plugin->dialog_state = NULL;
    }
    
    G_OBJECT_CLASS(asusd_battery_plugin_parent_class)->finalize(object);
}

static void asusd_battery_plugin_class_init(AsusdBatteryPluginClass *klass) {
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = asusd_battery_plugin_dispose;
    object_class->finalize = asusd_battery_plugin_finalize;
}

static void asusd_battery_plugin_init(AsusdBatteryPlugin *plugin) {
    plugin->current_profile = g_strdup("balanced");
    plugin->asusd_state = ASUSD_STATE_UNAVAILABLE;
    plugin->hide_icon = FALSE;
    plugin->hide_text = FALSE;
    plugin->hide_notifications = FALSE;
    plugin->is_on_ac = TRUE;
    plugin->last_notification_time = 0;
    plugin->settings_dialog_open = FALSE;
    plugin->saving_settings = FALSE;
    plugin->dialog_state = NULL;
    plugin->dialog_id_counter = 0;
    plugin->asusd_proxy = NULL;
    plugin->upower_proxy = NULL;
    plugin->connection = NULL;
    plugin->asusd_init_retry_count = 0;
    plugin->init_load_state = 0;
    plugin->pending_calls = 0;
    plugin->processing_ops = FALSE;
    plugin->is_disposing = FALSE;
    plugin->cancellable = g_cancellable_new();
    plugin->operation_queue = g_queue_new();
    plugin->profiles = g_ptr_array_new_with_free_func((GDestroyNotify)profile_settings_free);
    plugin->profile_lookup = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);
}

/* ========== Обновление отображения ========== */

void update_profile_display(AsusdBatteryPlugin *plugin, gboolean should_notify) {
    if (!plugin || plugin->is_disposing) return;
    
    if (!plugin->profiles || plugin->profiles->len == 0) {
        DEBUG_DEBUG("update_profile_display: No profiles loaded, creating fallback");
        create_fallback_profiles(plugin);
    }
    
    const gchar *profile = plugin->current_profile ? plugin->current_profile : "balanced";
    if (g_strcmp0(profile, "unknown") == 0) {
        profile = "balanced";
        DEBUG_DEBUG("update_profile_display: profile was 'unknown', using 'balanced'");
    }
    
    DEBUG_DEBUG("=== update_profile_display ===");
    DEBUG_DEBUG("  should_notify = %d", should_notify);
    DEBUG_DEBUG("  hide_notifications = %d", plugin->hide_notifications);
    DEBUG_DEBUG("  profile = '%s'", profile);
    DEBUG_DEBUG("  last_displayed_profile = '%s'", plugin->last_displayed_profile ? plugin->last_displayed_profile : "NULL");
    
    gboolean profile_changed = FALSE;
    if (should_notify && !plugin->hide_notifications && plugin->asusd_state == ASUSD_STATE_AVAILABLE) {
        if (plugin->last_displayed_profile == NULL || g_strcmp0(plugin->last_displayed_profile, profile) != 0)
            profile_changed = TRUE;
    }
    
    g_free(plugin->last_displayed_profile);
    plugin->last_displayed_profile = g_strdup(profile);
    
    if (plugin->hide_icon) gtk_widget_hide(plugin->image);
    else gtk_widget_show(plugin->image);
    if (plugin->hide_text) gtk_widget_hide(plugin->label);
    else gtk_widget_show(plugin->label);
    
    if (profile && g_strcmp0(profile, "unknown") != 0) {
        const gchar *icon_name = NULL;
        if (plugin->profile_lookup) {
            GHashTableIter iter;
            gpointer key, value;
            g_hash_table_iter_init(&iter, plugin->profile_lookup);
            while (g_hash_table_iter_next(&iter, &key, &value)) {
                ProfileSettings *settings = (ProfileSettings*)value;
                if (settings->name && g_strcmp0(profile, settings->name) == 0) {
                    if (settings->icon && strlen(settings->icon) > 0) icon_name = settings->icon;
                    break;
                }
                if (settings->default_name && g_strcmp0(profile, settings->default_name) == 0) {
                    if (settings->icon && strlen(settings->icon) > 0) icon_name = settings->icon;
                    break;
                }
            }
        }
        
        if (!plugin->hide_text) {
            gchar *display_text = g_strdup(profile);
            if (g_strcmp0(profile, "balanced") == 0 || g_strcmp0(profile, "performance") == 0 || g_strcmp0(profile, "quiet") == 0) {
                if (strlen(display_text) > 0) display_text[0] = g_ascii_toupper(display_text[0]);
            }
            gtk_label_set_text(GTK_LABEL(plugin->label), display_text);
            g_free(display_text);
        } else {
            gtk_label_set_text(GTK_LABEL(plugin->label), "");
        }
        if (icon_name) gtk_image_set_from_icon_name(GTK_IMAGE(plugin->image), icon_name, GTK_ICON_SIZE_MENU);
        else gtk_image_set_from_icon_name(GTK_IMAGE(plugin->image), get_profile_icon(plugin, profile), GTK_ICON_SIZE_MENU);
    } else {
        gtk_label_set_text(GTK_LABEL(plugin->label), _("Balanced"));
        gtk_image_set_from_icon_name(GTK_IMAGE(plugin->image), "battery-good-symbolic", GTK_ICON_SIZE_MENU);
    }
    
    if (profile_changed && profile && g_strcmp0(profile, "unknown") != 0 && can_send_notification(plugin)) {
        DEBUG_DEBUG("  >>> SENDING NOTIFICATION for profile: %s", profile);
        gchar *display_name = g_strdup(profile);
        if (display_name[0] >= 'a' && display_name[0] <= 'z') display_name[0] = g_ascii_toupper(display_name[0]);
        const gchar *icon = get_profile_icon(plugin, profile);
        gchar *subtitle = g_strdup_printf(_("Current profile: %s"), display_name);
        send_notification(_("Performance profile changed"), subtitle, FALSE, icon);
        g_free(subtitle);
        g_free(display_name);
    }
    DEBUG_DEBUG("=== end update_profile_display ===");
}

/* ========== Настройки ========== */

void load_settings(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    XfconfChannel *channel = xfconf_channel_get(CONFIG_CHANNEL);
    if (!channel) { DEBUG_WARN("Failed to get xfconf channel"); return; }
    plugin->hide_icon = xfconf_channel_get_bool(channel, CONFIG_PROPERTY_PREFIX "/hide_icon", FALSE);
    plugin->hide_text = xfconf_channel_get_bool(channel, CONFIG_PROPERTY_PREFIX "/hide_text", FALSE);
    plugin->hide_notifications = xfconf_channel_get_bool(channel, CONFIG_PROPERTY_PREFIX "/hide_notifications", FALSE);
}

void save_settings(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    XfconfChannel *channel = xfconf_channel_get(CONFIG_CHANNEL);
    if (!channel) { DEBUG_WARN("Failed to get xfconf channel"); return; }
    xfconf_channel_set_bool(channel, CONFIG_PROPERTY_PREFIX "/hide_icon", plugin->hide_icon);
    xfconf_channel_set_bool(channel, CONFIG_PROPERTY_PREFIX "/hide_text", plugin->hide_text);
    xfconf_channel_set_bool(channel, CONFIG_PROPERTY_PREFIX "/hide_notifications", plugin->hide_notifications);
    if (plugin->profiles) {
        for (guint i = 0; i < plugin->profiles->len; i++) {
            ProfileSettings *settings = g_ptr_array_index(plugin->profiles, i);
            gchar *key = g_strdup_printf("%s/profile_%d_name", CONFIG_PROPERTY_PREFIX, settings->enum_value);
            if (settings->name && strlen(settings->name) > 0)
                xfconf_channel_set_string(channel, key, settings->name);
            else
                xfconf_channel_set_string(channel, key, "");
            g_free(key);
            key = g_strdup_printf("%s/profile_%d_icon", CONFIG_PROPERTY_PREFIX, settings->enum_value);
            if (settings->icon && strlen(settings->icon) > 0)
                xfconf_channel_set_string(channel, key, settings->icon);
            else
                xfconf_channel_set_string(channel, key, "");
            g_free(key);
        }
    }
}

/* ========== Интернационализация ========== */

void init_i18n(void) {
    bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);
}

/* ========== UI Callbacks ========== */

void on_button_clicked(GtkWidget *widget, AsusdBatteryPlugin *plugin) {
    if (!plugin || plugin->is_disposing) return;
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) return;
    
    GtkWidget *menu = gtk_menu_new();
    if (!menu) return;
    gchar **profiles = asusd_get_available_profiles(plugin);
    const gchar *current = plugin->current_profile ? plugin->current_profile : "balanced";
    
    if (profiles) {
        for (int i = 0; profiles[i] != NULL; i++) {
            gchar *icon_name = NULL;
            GHashTableIter iter;
            gpointer key, value;
            g_hash_table_iter_init(&iter, plugin->profile_lookup);
            while (g_hash_table_iter_next(&iter, &key, &value)) {
                ProfileSettings *settings = (ProfileSettings*)value;
                if (settings->name && g_strcmp0(profiles[i], settings->name) == 0) {
                    if (settings->icon && strlen(settings->icon) > 0) icon_name = g_strdup(settings->icon);
                    break;
                }
                if (settings->default_name && g_strcmp0(profiles[i], settings->default_name) == 0) {
                    if (settings->icon && strlen(settings->icon) > 0) icon_name = g_strdup(settings->icon);
                    break;
                }
            }
            
            GtkWidget *item = gtk_check_menu_item_new();
            GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            if (icon_name) {
                GtkWidget *image = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
                gtk_box_pack_start(GTK_BOX(hbox), image, FALSE, FALSE, 0);
                g_free(icon_name);
            }
            GtkWidget *label = gtk_label_new(profiles[i]);
            gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
            gtk_container_add(GTK_CONTAINER(item), hbox);
            gtk_widget_show_all(hbox);
            if (g_strcmp0(profiles[i], current) == 0) {
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
                gtk_widget_set_sensitive(item, FALSE);
            }
            g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(on_profile_selected), plugin);
            g_object_set_data_full(G_OBJECT(item), "profile", g_strdup(profiles[i]), g_free);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        }
        g_strfreev(profiles);
    }
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_widget(GTK_MENU(menu), plugin->button, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH, NULL);
}

void on_profile_selected(GtkMenuItem *item, AsusdBatteryPlugin *plugin) {
    if (!plugin || plugin->is_disposing) return;
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) return;
    gchar *profile = (gchar *)g_object_get_data(G_OBJECT(item), "profile");
    if (!profile) return;
    asusd_set_profile_async(plugin, profile, (GAsyncReadyCallback)on_set_profile_done, plugin);
}

void on_set_profile_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = get_plugin_ref(user_data);
    if (!plugin || plugin->is_disposing) {
        if (plugin) g_object_unref(plugin);
        return;
    }
    
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error) {
        DEBUG_WARN("Failed to set profile: %s", error->message);
        if (!plugin->hide_notifications)
            send_notification(_("Error changing profile"), _("Failed to set profile via ASUSD"), TRUE, "emblem-readonly");
        g_error_free(error);
        g_object_unref(plugin);
        return;
    }
    if (result) g_variant_unref(result);
    DEBUG_DEBUG("Profile set successfully, waiting for property change signal");
    g_object_unref(plugin);
}

gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, AsusdBatteryPlugin *plugin) {
    if (!plugin || plugin->is_disposing) return FALSE;
    if (event->button == 3) {
        GtkWidget *menu = gtk_menu_new();
        if (!menu) return FALSE;
        GtkWidget *item = gtk_menu_item_new_with_label(_("Settings"));
        if (item) { g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(on_menu_configure), plugin); gtk_menu_shell_append(GTK_MENU_SHELL(menu), item); }
        item = gtk_separator_menu_item_new();
        if (item) gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        item = gtk_menu_item_new_with_label(_("About"));
        if (item) { g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(on_menu_about), plugin); gtk_menu_shell_append(GTK_MENU_SHELL(menu), item); }
        gtk_widget_show_all(menu);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent*)event);
        return TRUE;
    }
    return FALSE;
}

void on_menu_configure(GtkMenuItem *item, AsusdBatteryPlugin *plugin) { 
    if (!plugin || plugin->is_disposing) return; 
    create_settings_dialog(plugin); 
}

void on_menu_about(GtkMenuItem *item, AsusdBatteryPlugin *plugin) { 
    if (!plugin || plugin->is_disposing) return; 
    create_about_dialog(plugin); 
}

/* ========== Создание плагина ========== */

void asusd_battery_plugin_construct(XfcePanelPlugin *plugin) {
    debug_init();
    DEBUG_TRACE_ENTER();
    DEBUG_INFO("Initializing ASUS Battery plugin v%s", VERSION);

    AsusdBatteryPlugin *plugin_data;
    GError *error = NULL;
    init_i18n();
    if (!xfconf_init(&error)) {
        DEBUG_WARN("Failed to initialize xfconf: %s", error ? error->message : "unknown");
        if (error) g_error_free(error);
        return;
    }
    
    plugin_data = g_object_new(ASUSD_TYPE_BATTERY_PLUGIN, NULL);
    if (!plugin_data) { DEBUG_WARN("Failed to allocate memory"); return; }
    
    plugin_data->plugin = plugin;
    load_settings(plugin_data);
    create_fallback_profiles(plugin_data);
    
    plugin_data->button = gtk_button_new();
    if (!plugin_data->button) { DEBUG_WARN("Failed to create button"); g_object_unref(plugin_data); return; }
    gtk_button_set_relief(GTK_BUTTON(plugin_data->button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(plugin_data->button, FALSE);
    gtk_widget_set_tooltip_text(plugin_data->button, _("Manage performance profile"));
    plugin_data->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_add(GTK_CONTAINER(plugin_data->button), plugin_data->box);
    plugin_data->image = gtk_image_new_from_icon_name("battery-good-symbolic", GTK_ICON_SIZE_MENU);
    gtk_box_pack_start(GTK_BOX(plugin_data->box), plugin_data->image, FALSE, FALSE, 0);
    plugin_data->label = gtk_label_new(NULL);
    gtk_box_pack_start(GTK_BOX(plugin_data->box), plugin_data->label, FALSE, FALSE, 0);
    
    update_profile_display(plugin_data, FALSE);
    asusd_init_async(plugin_data);
    
    g_signal_connect(G_OBJECT(plugin_data->button), "clicked", G_CALLBACK(on_button_clicked), plugin_data);
    g_signal_connect(G_OBJECT(plugin_data->button), "button-press-event", G_CALLBACK(on_button_press), plugin_data);
    gtk_container_add(GTK_CONTAINER(plugin), plugin_data->button);
    gtk_widget_show_all(plugin_data->button);
    create_upower_proxy_async(plugin_data);
    
    g_object_set_data_full(G_OBJECT(plugin), "plugin_data", plugin_data, (GDestroyNotify)g_object_unref);
}

XFCE_PANEL_PLUGIN_REGISTER(asusd_battery_plugin_construct)
