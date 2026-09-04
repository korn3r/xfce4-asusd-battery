/* src/main.c */
#include "plugin.h"
#include "utils.h"
#include "profile-manager.h"
#include "asusd-client.h"
#include "settings-dialog.h"
#include "config.h"

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>
#include <libxfce4util/libxfce4util.h>
#include <xfconf/xfconf.h>
#include "debug.h"

/* Константы состояния ASUSD (если не определены в plugin.h) */
#ifndef ASUSD_STATE_UNAVAILABLE
#define ASUSD_STATE_UNAVAILABLE 0
#define ASUSD_STATE_CONNECTING  1
#define ASUSD_STATE_AVAILABLE   2
#endif

/* ========== Forward declarations ========== */

/* UI Callbacks */
void on_button_clicked(GtkWidget *widget, AsusdBatteryPlugin *plugin);
void on_profile_selected(GtkMenuItem *item, AsusdBatteryPlugin *plugin);
void on_set_profile_done(GObject *source, GAsyncResult *res, gpointer user_data);
void on_menu_configure(GtkMenuItem *item, AsusdBatteryPlugin *plugin);
void on_menu_about(GtkMenuItem *item, AsusdBatteryPlugin *plugin);

/* About dialog */
void create_about_dialog(AsusdBatteryPlugin *plugin);

/* ========== GObject определения ========== */

G_DEFINE_TYPE(AsusdBatteryPlugin, asusd_battery_plugin, XFCE_TYPE_PANEL_PLUGIN)

static void asusd_battery_plugin_about(XfcePanelPlugin *plugin) {
    AsusdBatteryPlugin *plugin_data = g_object_get_data(G_OBJECT(plugin), "plugin_data");
    if (plugin_data) {
        create_about_dialog(plugin_data);
    }
}

static void asusd_battery_plugin_configure(XfcePanelPlugin *plugin) {
    AsusdBatteryPlugin *plugin_data = g_object_get_data(G_OBJECT(plugin), "plugin_data");
    if (plugin_data) {
        create_settings_dialog(plugin_data);
    }
}

static void asusd_battery_plugin_class_init(AsusdBatteryPluginClass *klass) {
    XfcePanelPluginClass *plugin_class = XFCE_PANEL_PLUGIN_CLASS(klass);
    
    /* Переопределяем виртуальные методы (как в xkb-plugin) */
    plugin_class->about = asusd_battery_plugin_about;
    plugin_class->configure_plugin = asusd_battery_plugin_configure;
}

static void asusd_battery_plugin_init(AsusdBatteryPlugin *plugin) {
    /* Инициализация полей */
    plugin->current_profile = g_strdup("balanced");
    plugin->asusd_state = ASUSD_STATE_UNAVAILABLE;
    plugin->hide_icon = FALSE;
    plugin->hide_text = FALSE;
    plugin->hide_notifications = FALSE;
    plugin->enable_antiflapping = FALSE;
    plugin->custom_time_enabled = FALSE;
    plugin->custom_timeout_ms = 1500;
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
    plugin->asusd_retry_timeout_id = 0;
    plugin->reconnecting = FALSE;
    plugin->notification_timeout_id = 0;
    plugin->pending_notification_profile = NULL;
    plugin->last_notified_profile = NULL;
}


/* Отправка уведомления с задержкой 2 секунды */
static gboolean send_delayed_notification(gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin*)user_data;
    if (!plugin || plugin->is_disposing) return G_SOURCE_REMOVE;
    
    plugin->notification_timeout_id = 0;
    
    if (plugin->hide_notifications) {
        g_free(plugin->pending_notification_profile);
        plugin->pending_notification_profile = NULL;
        DEBUG_TRACE("  >>> NOTIFICATION CANCELLED: notifications are hidden");
        return G_SOURCE_REMOVE;
    }
    
    if (plugin->pending_notification_profile) {
        const gchar *current_profile = plugin->current_profile ? plugin->current_profile : "balanced";
        const gchar *pending = plugin->pending_notification_profile;
        
        /* Сравниваем ПОЛНЫЕ имена профилей */
        if (g_strcmp0(current_profile, pending) == 0) {
            /* Ищем пользовательское имя для профиля */
            const gchar *display_name = pending;
            if (plugin->profile_lookup) {
                GHashTableIter iter;
                gpointer key, value;
                g_hash_table_iter_init(&iter, plugin->profile_lookup);
                while (g_hash_table_iter_next(&iter, &key, &value)) {
                    ProfileSettings *settings = (ProfileSettings*)value;
                    if (settings->default_name && g_strcmp0(pending, settings->default_name) == 0) {
                        if (settings->name && strlen(settings->name) > 0) {
                            display_name = settings->name;
                        }
                        break;
                    }
                }
            }
            
            const gchar *icon = get_profile_icon(plugin, pending);
            gchar *subtitle = g_strdup_printf(_("Current profile: %s"), display_name);
            send_notification(_("Performance profile changed"), subtitle, FALSE, icon);
            g_free(subtitle);
            
            g_free(plugin->last_notified_profile);
            plugin->last_notified_profile = g_strdup(pending);
            DEBUG_TRACE("  >>> NOTIFICATION SENT for profile: %s (display: %s)", pending, display_name);
        } else {
            DEBUG_TRACE("  >>> NOTIFICATION CANCELLED: profile changed from '%s' to '%s' during delay", 
                        pending, current_profile);
        }
        
        g_free(plugin->pending_notification_profile);
        plugin->pending_notification_profile = NULL;
    }
    
    return G_SOURCE_REMOVE;
}

/* Запланировать уведомление с задержкой */
void schedule_notification(AsusdBatteryPlugin *plugin, const gchar *profile) {
    if (!plugin || !profile) return;
    
    if (plugin->hide_notifications) {
        DEBUG_TRACE("  >>> NOTIFICATION SKIPPED: notifications are hidden");
        return;
    }
    
    /* Если anti-flapping выключен — отправляем уведомление сразу */
    if (!plugin->enable_antiflapping) {
        /* Ищем пользовательское имя для профиля */
        const gchar *display_name = profile;
        if (plugin->profile_lookup) {
            GHashTableIter iter;
            gpointer key, value;
            g_hash_table_iter_init(&iter, plugin->profile_lookup);
            while (g_hash_table_iter_next(&iter, &key, &value)) {
                ProfileSettings *settings = (ProfileSettings*)value;
                if (settings->default_name && g_strcmp0(profile, settings->default_name) == 0) {
                    if (settings->name && strlen(settings->name) > 0) {
                        display_name = settings->name;
                    }
                    break;
                }
            }
        }
        
        const gchar *icon = get_profile_icon(plugin, profile);
        gchar *subtitle = g_strdup_printf(_("Current profile: %s"), display_name);
        send_notification(_("Performance profile changed"), subtitle, FALSE, icon);
        g_free(subtitle);
        return;
    }
    
    /* ===== Anti-flapping включен ===== */
    
    /* Если уже есть запланированное уведомление для ТОГО ЖЕ профиля — не отменяем */
    if (plugin->pending_notification_profile && 
        g_strcmp0(plugin->pending_notification_profile, profile) == 0) {
        DEBUG_TRACE("  >>> NOTIFICATION ALREADY SCHEDULED for profile: %s, skipping", profile);
        return;
    }
    
    /* Отменяем предыдущий таймер, если он есть */
    if (plugin->notification_timeout_id > 0) {
        g_source_remove(plugin->notification_timeout_id);
        plugin->notification_timeout_id = 0;
        DEBUG_TRACE("  >>> Previous notification cancelled (new profile: %s)", profile);
    }
    
    /* Сохраняем новый профиль для уведомления */
    g_free(plugin->pending_notification_profile);
    plugin->pending_notification_profile = g_strdup(profile);
    
    /* Определяем таймаут: если Custom time включен — используем custom_timeout_ms, иначе DEFAULT_TIMEOUT_MS */
    guint timeout_ms;
    if (plugin->custom_time_enabled) {
        timeout_ms = plugin->custom_timeout_ms;
        if (timeout_ms == 0) {
            timeout_ms = DEFAULT_TIMEOUT_MS;
        }
    } else {
        timeout_ms = DEFAULT_TIMEOUT_MS;
    }
    
    plugin->notification_timeout_id = g_timeout_add(timeout_ms, send_delayed_notification, plugin);
    DEBUG_TRACE("  >>> SCHEDULED notification in %u ms for profile: %s (custom_time_enabled=%d)", 
                timeout_ms, profile, plugin->custom_time_enabled);
}

void update_profile_display(AsusdBatteryPlugin *plugin, gboolean should_notify) {
    if (!plugin || plugin->is_disposing) return;
    
    if (!plugin->profiles || plugin->profiles->len == 0) {
        DEBUG_TRACE("update_profile_display: No profiles loaded, creating fallback");
        create_fallback_profiles(plugin);
    }
    
    const gchar *profile = plugin->current_profile ? plugin->current_profile : "balanced";
    const gchar *display_profile = profile;
    
    /* Ищем кастомное имя для профиля по default_name (ID) */
    if (plugin->profile_lookup && profile && g_strcmp0(profile, "unknown") != 0) {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, plugin->profile_lookup);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            ProfileSettings *settings = (ProfileSettings*)value;
            if (settings->default_name && g_strcmp0(profile, settings->default_name) == 0) {
                if (settings->name && strlen(settings->name) > 0) {
                    display_profile = settings->name;
                }
                break;
            }
        }
    }
    
    if (g_strcmp0(profile, "unknown") == 0) {
        profile = "balanced";
        display_profile = "balanced";
        DEBUG_TRACE("update_profile_display: profile was 'unknown', using 'balanced'");
    }
    
    DEBUG_TRACE("=== update_profile_display ===");
    DEBUG_TRACE("  should_notify = %d", should_notify);
    DEBUG_TRACE("  hide_notifications = %d", plugin->hide_notifications);
    DEBUG_TRACE("  profile = '%s'", profile);
    DEBUG_TRACE("  display_profile = '%s'", display_profile ? display_profile : "NULL");
    DEBUG_TRACE("  last_displayed_profile = '%s'", plugin->last_displayed_profile ? plugin->last_displayed_profile : "NULL");
    DEBUG_TRACE("  current_profile = '%s'", plugin->current_profile ? plugin->current_profile : "NULL");
    
    gboolean profile_changed = FALSE;
    if (should_notify && !plugin->hide_notifications && plugin->asusd_state == ASUSD_STATE_AVAILABLE) {
        if (plugin->last_displayed_profile == NULL || g_strcmp0(plugin->last_displayed_profile, profile) != 0) {
            profile_changed = TRUE;
            DEBUG_TRACE("  profile_changed: last='%s' -> new='%s'", 
                        plugin->last_displayed_profile ? plugin->last_displayed_profile : "NULL", 
                        profile);
        } else {
            DEBUG_TRACE("  profile_changed: last='%s' == new='%s' (NO CHANGE)", 
                        plugin->last_displayed_profile ? plugin->last_displayed_profile : "NULL", 
                        profile);
        }
    }
    
    g_free(plugin->last_displayed_profile);
    plugin->last_displayed_profile = g_strdup(profile);
    
    /* ===== Применяем настройки видимости ===== */
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
    
    /* ===== Настройки Fixed width, Right icon, Align ===== */
    
    /* Устанавливаем ширину кнопки, если включено Fixed width */
    if (plugin->fixed_width_enabled && plugin->fixed_width_value > 0) {
        gtk_widget_set_size_request(plugin->button, plugin->fixed_width_value, -1);
        DEBUG_TRACE("  fixed_width: %d", plugin->fixed_width_value);
    } else {
        gtk_widget_set_size_request(plugin->button, -1, -1);
    }
    
    /* Устанавливаем выравнивание содержимого */
    GtkWidget *parent = gtk_widget_get_parent(plugin->box);
    if (parent == plugin->button) {
        if (plugin->align == 0) { /* Left */
            gtk_widget_set_halign(plugin->box, GTK_ALIGN_START);
        } else if (plugin->align == 1) { /* Center */
            gtk_widget_set_halign(plugin->box, GTK_ALIGN_CENTER);
        } else { /* Right */
            gtk_widget_set_halign(plugin->box, GTK_ALIGN_END);
        }
        DEBUG_TRACE("  align: %d", plugin->align);
    }
    
    /* Применяем Right icon — меняем порядок упаковки */
    if (plugin->right_icon && !plugin->hide_icon) {
        /* Перемещаем иконку в конец (справа) */
        gtk_box_reorder_child(GTK_BOX(plugin->box), plugin->image, -1);
        gtk_box_reorder_child(GTK_BOX(plugin->box), plugin->label, 0);
        DEBUG_TRACE("  right_icon: TRUE");
    } else {
        /* Возвращаем иконку в начало (слева) */
        gtk_box_reorder_child(GTK_BOX(plugin->box), plugin->image, 0);
        gtk_box_reorder_child(GTK_BOX(plugin->box), plugin->label, 1);
        DEBUG_TRACE("  right_icon: FALSE");
    }
    
    if (profile && g_strcmp0(profile, "unknown") != 0) {
        const gchar *icon_name = NULL;
        if (plugin->profile_lookup) {
            GHashTableIter iter;
            gpointer key, value;
            g_hash_table_iter_init(&iter, plugin->profile_lookup);
            while (g_hash_table_iter_next(&iter, &key, &value)) {
                ProfileSettings *settings = (ProfileSettings*)value;
                if (settings->default_name && g_strcmp0(profile, settings->default_name) == 0) {
                    if (settings->icon && strlen(settings->icon) > 0) icon_name = settings->icon;
                    break;
                }
            }
        }
        
        DEBUG_TRACE("  icon_name = '%s'", icon_name ? icon_name : "NULL");
        DEBUG_TRACE("  fallback icon = '%s'", get_profile_icon(plugin, profile));
        
        if (!plugin->hide_text) {
            gchar *display_text = g_strdup(display_profile);
            if (g_strcmp0(display_profile, "balanced") == 0 || 
                g_strcmp0(display_profile, "performance") == 0 || 
                g_strcmp0(display_profile, "quiet") == 0 ||
                g_strcmp0(display_profile, "powersave") == 0) {
                if (strlen(display_text) > 0) display_text[0] = g_ascii_toupper(display_text[0]);
            }
            gtk_label_set_text(GTK_LABEL(plugin->label), display_text);
            g_free(display_text);
        } else {
            gtk_label_set_text(GTK_LABEL(plugin->label), "");
        }
        if (icon_name) {
            gtk_image_set_from_icon_name(GTK_IMAGE(plugin->image), icon_name, GTK_ICON_SIZE_MENU);
        } else {
            gtk_image_set_from_icon_name(GTK_IMAGE(plugin->image), get_profile_icon(plugin, profile), GTK_ICON_SIZE_MENU);
        }
    } else {
        gtk_label_set_text(GTK_LABEL(plugin->label), _("Balanced"));
        gtk_image_set_from_icon_name(GTK_IMAGE(plugin->image), "battery-good-symbolic", GTK_ICON_SIZE_MENU);
    }
    
    /* Еще раз применяем настройки видимости после установки иконки/текста */
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
    
    if (profile_changed && profile && g_strcmp0(profile, "unknown") != 0 && can_send_notification(plugin)) {
        DEBUG_TRACE("  >>> SCHEDULING NOTIFICATION for profile: %s", profile);
        schedule_notification(plugin, profile);
    } else {
        if (!profile_changed) {
            DEBUG_TRACE("  >>> NOTIFICATION NOT SCHEDULED: profile_changed=FALSE");
        } else if (!can_send_notification(plugin)) {
            DEBUG_TRACE("  >>> NOTIFICATION NOT SCHEDULED: can_send_notification=FALSE");
        }
    }
    
    DEBUG_TRACE("=== end update_profile_display ===");
}

/* ========== Настройки ========== */

void load_settings(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    
    XfconfChannel *channel = xfconf_channel_get(CONFIG_CHANNEL);
    if (!channel) {
        DEBUG_WARN("Failed to get xfconf channel");
        return;
    }
    
    /* Display options */
    plugin->hide_icon = xfconf_channel_get_bool(channel, "/hide_icon", FALSE);
    plugin->hide_text = xfconf_channel_get_bool(channel, "/hide_text", FALSE);
    plugin->hide_notifications = xfconf_channel_get_bool(channel, "/hide_notifications", FALSE);
    
    /* Новые настройки Display options */
    plugin->fixed_width_enabled = xfconf_channel_get_bool(channel, KEY_FIXED_WIDTH_ENABLED, DEFAULT_FIXED_WIDTH_ENABLED);
    plugin->fixed_width_value = xfconf_channel_get_uint(channel, KEY_FIXED_WIDTH_VALUE, DEFAULT_FIXED_WIDTH_VALUE);
    plugin->right_icon = xfconf_channel_get_bool(channel, KEY_RIGHT_ICON, DEFAULT_RIGHT_ICON);
    plugin->align = xfconf_channel_get_int(channel, KEY_ALIGN, DEFAULT_ALIGN);
    
    /* Anti-flapping settings */
    plugin->enable_antiflapping = xfconf_channel_get_bool(channel, "/enable_antiflapping", FALSE);
    plugin->custom_time_enabled = xfconf_channel_get_bool(channel, "/custom_time_enabled", FALSE);
    plugin->custom_timeout_ms = xfconf_channel_get_uint(channel, "/custom_timeout_ms", DEFAULT_TIMEOUT_MS);
    if (plugin->custom_timeout_ms == 0) {
        plugin->custom_timeout_ms = DEFAULT_TIMEOUT_MS;
    }
    
    /* No battery */
    plugin->no_battery = xfconf_channel_get_bool(channel, "/no_battery", FALSE);
    
    /* Загружаем пользовательские имена и иконки */
    if (plugin->profiles) {
        for (guint i = 0; i < plugin->profiles->len; i++) {
            ProfileSettings *settings = g_ptr_array_index(plugin->profiles, i);
            
            gchar *key = g_strdup_printf("/profile_%d_name", settings->enum_value);
            gchar *saved_name = xfconf_channel_get_string(channel, key, NULL);
            if (saved_name && strlen(saved_name) > 0) {
                g_free(settings->name);
                settings->name = g_strdup(saved_name);
            }
            g_free(key);
            g_free(saved_name);
            
            key = g_strdup_printf("/profile_%d_icon", settings->enum_value);
            gchar *saved_icon = xfconf_channel_get_string(channel, key, NULL);
            if (saved_icon && strlen(saved_icon) > 0) {
                g_free(settings->icon);
                settings->icon = g_strdup(saved_icon);
            }
            g_free(key);
            g_free(saved_icon);
        }
    }
}

void save_settings(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    
    XfconfChannel *channel = xfconf_channel_get(CONFIG_CHANNEL);
    if (!channel) {
        DEBUG_WARN("Failed to get xfconf channel");
        return;
    }
    
    /* Display options */
    xfconf_channel_set_bool(channel, "/hide_icon", plugin->hide_icon);
    xfconf_channel_set_bool(channel, "/hide_text", plugin->hide_text);
    xfconf_channel_set_bool(channel, "/hide_notifications", plugin->hide_notifications);
    
    /* Новые настройки Display options */
    xfconf_channel_set_bool(channel, KEY_FIXED_WIDTH_ENABLED, plugin->fixed_width_enabled);
    xfconf_channel_set_uint(channel, KEY_FIXED_WIDTH_VALUE, plugin->fixed_width_value);
    xfconf_channel_set_bool(channel, KEY_RIGHT_ICON, plugin->right_icon);
    xfconf_channel_set_int(channel, KEY_ALIGN, plugin->align);
    
    /* Anti-flapping settings */
    xfconf_channel_set_bool(channel, "/enable_antiflapping", plugin->enable_antiflapping);
    xfconf_channel_set_bool(channel, "/custom_time_enabled", plugin->custom_time_enabled);
    xfconf_channel_set_uint(channel, "/custom_timeout_ms", plugin->custom_timeout_ms);
    xfconf_channel_set_bool(channel, "/no_battery", plugin->no_battery);
    
    /* Сохраняем пользовательские имена и иконки */
    if (plugin->profiles) {
        for (guint i = 0; i < plugin->profiles->len; i++) {
            ProfileSettings *settings = g_ptr_array_index(plugin->profiles, i);
            
            gchar *key = g_strdup_printf("/profile_%d_name", settings->enum_value);
            if (settings->name && strlen(settings->name) > 0) {
                xfconf_channel_set_string(channel, key, settings->name);
            } else {
                xfconf_channel_set_string(channel, key, "");
            }
            g_free(key);
            
            key = g_strdup_printf("/profile_%d_icon", settings->enum_value);
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
                xfconf_channel_set_string(channel, key, settings->icon);
            } else {
                xfconf_channel_set_string(channel, key, "");
            }
            g_free(key);
        }
    }
}

/* ========== UI Callbacks ========== */

void on_button_clicked(G_GNUC_UNUSED GtkWidget *widget, AsusdBatteryPlugin *plugin) {
    if (!plugin || plugin->is_disposing) return;
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) return;
    
    GtkWidget *menu = gtk_menu_new();
    if (!menu) return;
    
    /* Получаем список профилей с их enum-значениями */
    GPtrArray *profiles = plugin->profiles;
    if (!profiles || profiles->len == 0) {
        create_fallback_profiles(plugin);
        profiles = plugin->profiles;
        if (!profiles || profiles->len == 0) return;
    }
    
    /* Получаем текущий активный профиль (enum) */
    guint32 current_enum = 999;
    if (plugin->current_profile) {
        if (!profile_enum_from_name(plugin, plugin->current_profile, &current_enum)) {
            /* Если не нашли, пробуем по умолчанию */
            current_enum = 0; /* balanced */
        }
    }
    
    /* Сортируем профили для отображения */
    for (guint32 enum_val = 0; enum_val <= 10; enum_val++) {
        ProfileSettings *found = NULL;
        for (guint i = 0; i < profiles->len; i++) {
            ProfileSettings *s = g_ptr_array_index(profiles, i);
            if (s->enum_value == enum_val) {
                found = s;
                break;
            }
        }
        if (!found) continue;
        
        /* Используем отображаемое имя для пункта меню */
        const char *display_name = found->name && strlen(found->name) > 0 ? 
                                   found->name : found->default_name;
        
        GtkWidget *item = gtk_check_menu_item_new_with_label(display_name);
        
        /* АКТИВИРУЕМ по enum, а не по имени! */
        if (found->enum_value == current_enum) {
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
            gtk_widget_set_sensitive(item, FALSE);
        }
        
        /* Сохраняем enum-значение как ID профиля */
        g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(on_profile_selected), plugin);
        g_object_set_data(G_OBJECT(item), "profile_enum", GINT_TO_POINTER(found->enum_value));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_widget(GTK_MENU(menu), plugin->button, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH, NULL);
}

void on_profile_selected(GtkMenuItem *item, AsusdBatteryPlugin *plugin) {
    if (!plugin || plugin->is_disposing) return;
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) return;
    
    /* Получаем enum-значение, а не имя */
    guint32 enum_val = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(item), "profile_enum"));
    if (enum_val == 999) return; /* Невалидное значение */
    
    /* Получаем имя профиля по enum */
    const char *profile_name = profile_name_from_enum(plugin, enum_val);
    if (!profile_name) return;
    
    /* Сбрасываем last_notified_profile при ручном переключении */
    g_free(plugin->last_notified_profile);
    plugin->last_notified_profile = NULL;
    
    /* Отменяем отложенное уведомление */
    if (plugin->notification_timeout_id > 0) {
        g_source_remove(plugin->notification_timeout_id);
        plugin->notification_timeout_id = 0;
    }
    g_free(plugin->pending_notification_profile);
    plugin->pending_notification_profile = NULL;
    
    asusd_set_profile_async(plugin, profile_name, (GAsyncReadyCallback)on_set_profile_done, plugin);
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
    DEBUG_TRACE("Profile set successfully, waiting for property change signal");
    g_object_unref(plugin);
}

void on_menu_configure(G_GNUC_UNUSED GtkMenuItem *item, AsusdBatteryPlugin *plugin) { 
    if (!plugin || plugin->is_disposing) return; 
    create_settings_dialog(plugin); 
}

void on_menu_about(G_GNUC_UNUSED GtkMenuItem *item, AsusdBatteryPlugin *plugin) { 
    if (!plugin || plugin->is_disposing) return; 
    create_about_dialog(plugin); 
}

/* ========== About dialog ========== */

void create_about_dialog(AsusdBatteryPlugin *plugin) {
    if (!plugin || plugin->is_disposing) return;
    
    const char *authors[] = {
        "Deepseek, ChatGPT and korn3r",
        NULL
    };
    
    gtk_show_about_dialog(
        NULL,
        "program-name", "xfce4-asusd-battery",
        "version", VERSION,
        "comments", _("Battery and performance profile manager for ASUS laptops"),
        "authors", authors,
        "website", "https://github.com/korn3r/xfce4-asusd-battery",
        "logo-icon-name", "battery-good-symbolic",
        "license", "MIT License",
        NULL
    );
}

/* ========== Создание плагина ========== */

void asusd_battery_plugin_construct(XfcePanelPlugin *plugin) {
    debug_init();
    DEBUG_TRACE_ENTER();
    DEBUG_DEBUG("Initializing ASUS Battery plugin v%s", VERSION);

    DEBUG_DEBUG("xfce4-asusd-battery: D-Bus interface configuration:");
    DEBUG_DEBUG("  ASUSD_BUS_NAME       = %s", "xyz.ljones.Asusd");
    DEBUG_DEBUG("  ASUSD_OBJECT_PATH    = %s", "/xyz/ljones");
    DEBUG_DEBUG("  ASUSD_INTERFACE      = %s", "xyz.ljones.Platform");
    DEBUG_DEBUG("  DBUS_PROPERTIES      = %s", "org.freedesktop.DBus.Properties");

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
    
    /* 1. Сначала создаем профили по умолчанию */
    create_fallback_profiles(plugin_data);
    
    /* 2. Затем загружаем настройки из xfconf (перезаписывают значения по умолчанию) */
    load_settings(plugin_data);
    
    DEBUG_DEBUG("Loaded settings: hide_icon=%d, hide_text=%d, hide_notifications=%d", 
                plugin_data->hide_icon, plugin_data->hide_text, plugin_data->hide_notifications);
    
    /* 3. Создаем виджеты */
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
    
    /* 4. Сначала показываем все виджеты */
    gtk_widget_show_all(plugin_data->button);
    
    /* 5. Затем применяем настройки видимости (скрываем если нужно) */
    if (plugin_data->hide_icon) {
        gtk_widget_hide(plugin_data->image);
        DEBUG_DEBUG("Hiding icon at startup (hide_icon=%d)", plugin_data->hide_icon);
    }
    if (plugin_data->hide_text) {
        gtk_widget_hide(plugin_data->label);
        DEBUG_DEBUG("Hiding text at startup (hide_text=%d)", plugin_data->hide_text);
    }
    
    /* 6. Обновляем содержимое (текст и иконку) */
    update_profile_display(plugin_data, FALSE);
    
    /* 7. ЕЩЕ РАЗ применяем настройки видимости после update_profile_display */
    if (plugin_data->hide_icon) {
        gtk_widget_hide(plugin_data->image);
    }
    if (plugin_data->hide_text) {
        gtk_widget_hide(plugin_data->label);
    }
    
    /* 8. Добавляем стандартные пункты в меню плагина (как в xkb-plugin) */
    /* Добавляем стандартные пункты в меню плагина (как в xkb-plugin) */
    xfce_panel_plugin_menu_show_configure(plugin);
    xfce_panel_plugin_menu_show_about(plugin);
    
    /* Подключаем сигналы, чтобы пункты меню работали */
    g_signal_connect(plugin, "configure-plugin", 
                     G_CALLBACK(on_menu_configure), plugin_data);
    g_signal_connect(plugin, "about", 
                     G_CALLBACK(on_menu_about), plugin_data);
    
    /* Инициализируем ASUSD */
    asusd_init_async(plugin_data);
    
    g_signal_connect(G_OBJECT(plugin_data->button), "clicked", G_CALLBACK(on_button_clicked), plugin_data);
    gtk_container_add(GTK_CONTAINER(plugin), plugin_data->button);
    
    create_upower_proxy_async(plugin_data);
    
    g_object_set_data_full(G_OBJECT(plugin), "plugin_data", plugin_data, (GDestroyNotify)g_object_unref);
}

XFCE_PANEL_PLUGIN_REGISTER(asusd_battery_plugin_construct)
