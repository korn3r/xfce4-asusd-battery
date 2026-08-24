#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>
#include <libxfce4util/libxfce4util.h>
#include <xfconf/xfconf.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#define GETTEXT_PACKAGE "xfce4-asusd-battery"
#define LOCALEDIR "/usr/share/locale"

#include <gio/gio.h>

#define CONFIG_CHANNEL "xfce4-asusd-battery"
#define CONFIG_PROPERTY_PREFIX "/plugins/xfce4-asusd-battery"

/* ASUSD D-Bus константы */
#define ASUSD_BUS_NAME "xyz.ljones.Asusd"
#define ASUSD_OBJECT_PATH "/xyz/ljones"
#define ASUSD_INTERFACE "xyz.ljones.Platform"
#define DBUS_PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"
#define DBUS_NAME_OWNER_CHANGED "NameOwnerChanged"
#define ASUSD_TIMEOUT_MS 5000

/* Состояния ASUSD */
typedef enum {
    ASUSD_STATE_UNAVAILABLE,
    ASUSD_STATE_CONNECTING,
    ASUSD_STATE_AVAILABLE
} AsusdState;

typedef struct {
    gchar *name;
    gchar *icon;
    gchar *default_name;
    guint32 enum_value;
} ProfileSettings;

typedef struct {
    XfcePanelPlugin *plugin;
    GtkWidget *button;
    GtkWidget *box;
    GtkWidget *image;
    GtkWidget *label;
    
    /* Состояние профилей */
    gchar *current_profile;
    GPtrArray *profiles;          /* Упорядоченный список профилей */
    GHashTable *profile_lookup;   /* enum → ProfileSettings */
    
    /* Настройки отображения */
    gboolean hide_icon;
    gboolean hide_text;
    gboolean hide_notifications;  /* TRUE = уведомления скрыты (галочка включена) */
    
    /* Настройки ASUSD */
    gboolean battery_limit_enabled;
    gboolean auto_switch_ac_enabled;
    gboolean auto_switch_battery_enabled;
    gchar *auto_switch_ac_profile;
    gchar *auto_switch_battery_profile;
    guint current_battery_limit;
    
    /* D-Bus */
    GDBusConnection *dbus_connection;
    guint asusd_properties_signal_id;
    guint asusd_name_owner_signal_id;
    guint upower_properties_signal_id;
    gboolean is_on_ac;
    
    /* Состояние ASUSD */
    AsusdState asusd_state;
    guint asusd_retry_timeout_id;
    
    /* Для уведомлений */
    gchar *last_displayed_profile;
    time_t last_notification_time;
    
    /* Флаги для диалога настроек */
    gboolean settings_dialog_open;
    gboolean saving_settings;
} AsusdBatteryPlugin;

/* Прототипы */
static void asusd_battery_plugin_construct(XfcePanelPlugin *plugin);
static void asusd_battery_plugin_free(gpointer user_data);
static void update_profile_display(AsusdBatteryPlugin *plugin, gboolean should_notify);
static void on_button_clicked(GtkWidget *widget, AsusdBatteryPlugin *plugin);
static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, AsusdBatteryPlugin *plugin);
static void on_profile_selected(GtkMenuItem *item, AsusdBatteryPlugin *plugin);
static void on_menu_configure(GtkMenuItem *item, AsusdBatteryPlugin *plugin);
static void on_menu_about(GtkMenuItem *item, AsusdBatteryPlugin *plugin);
static void send_notification(const gchar *message, const gchar *subtitle, gboolean is_error, const gchar *icon);
static void load_settings(AsusdBatteryPlugin *plugin);
static void save_settings(AsusdBatteryPlugin *plugin);
static void create_settings_dialog(AsusdBatteryPlugin *plugin);
static void on_close_button_clicked(GtkButton *button, GtkWidget *dialog);
static void on_apply_clicked(GtkButton *button, AsusdBatteryPlugin *plugin);
static void on_dialog_destroy(GtkWidget *widget, AsusdBatteryPlugin *plugin);
static void on_any_setting_changed(GtkWidget *widget, AsusdBatteryPlugin *plugin);
static void on_hide_toggle(GtkToggleButton *toggle_button, AsusdBatteryPlugin *plugin);
static void on_auto_switch_toggled(GtkToggleButton *toggle_button, GtkWidget *dialog);
static void setup_upower_monitoring(AsusdBatteryPlugin *plugin);
static void on_dbus_signal(GDBusConnection *connection, const gchar *sender_name,
                           const gchar *object_path, const gchar *interface_name,
                           const gchar *signal_name, GVariant *parameters,
                           AsusdBatteryPlugin *plugin);
static void create_about_dialog(AsusdBatteryPlugin *plugin);
static void on_one_shot_clicked(GtkButton *button, GtkWidget *dialog);

/* ASUSD прототипы */
static gboolean asusd_init(AsusdBatteryPlugin *plugin);
static void asusd_cleanup(AsusdBatteryPlugin *plugin);
static gboolean asusd_set_profile(AsusdBatteryPlugin *plugin, const gchar *profile_name);
static void asusd_handle_properties_changed(AsusdBatteryPlugin *plugin, GVariant *changed_properties);
static void asusd_handle_name_owner_changed(AsusdBatteryPlugin *plugin, const gchar *old_owner, const gchar *new_owner);
static gboolean asusd_retry_init(gpointer user_data);
static gboolean asusd_setup_monitoring(AsusdBatteryPlugin *plugin);
static gboolean asusd_get_bool_property(AsusdBatteryPlugin *plugin, const char *property, gboolean *value);
static gboolean asusd_get_uint32_property(AsusdBatteryPlugin *plugin, const char *property, guint32 *value);
static gboolean asusd_get_byte_property(AsusdBatteryPlugin *plugin, const char *property, guint8 *value);
static gboolean asusd_set_property(AsusdBatteryPlugin *plugin, const char *property, GVariant *value);
static ProfileSettings* profile_settings_new(guint32 enum_value, const gchar *default_name);
static void profile_settings_free(ProfileSettings *settings);
static const gchar* get_profile_icon(AsusdBatteryPlugin *plugin, const gchar *profile);
static gboolean can_send_notification(AsusdBatteryPlugin *plugin);
static void reset_state(AsusdBatteryPlugin *plugin);
static const gchar* profile_name_from_enum(AsusdBatteryPlugin *plugin, guint32 enum_value);
static gboolean profile_enum_from_name(AsusdBatteryPlugin *plugin, const gchar *name, guint32 *enum_value);

/* ========== Вспомогательные функции ========== */

static void reset_state(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    
    plugin->asusd_state = ASUSD_STATE_UNAVAILABLE;
    g_free(plugin->current_profile);
    plugin->current_profile = g_strdup("unknown");
    
    plugin->battery_limit_enabled = FALSE;
    plugin->auto_switch_ac_enabled = FALSE;
    plugin->auto_switch_battery_enabled = FALSE;
    plugin->current_battery_limit = 0;
    
    update_profile_display(plugin, FALSE);
}

static const gchar* get_profile_icon(AsusdBatteryPlugin *plugin, const gchar *profile) {
    if (!plugin || !profile) return "battery-good-symbolic";
    
    /* Ищем в profile_lookup */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, plugin->profile_lookup);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        ProfileSettings *settings = (ProfileSettings*)value;
        if (settings->name && g_strcmp0(profile, settings->name) == 0) {
            if (settings->icon && strlen(settings->icon) > 0) {
                return settings->icon;
            }
            break;
        }
        if (settings->default_name && g_strcmp0(profile, settings->default_name) == 0) {
            if (settings->icon && strlen(settings->icon) > 0) {
                return settings->icon;
            }
            break;
        }
    }
    
    /* Стандартные иконки */
    if (g_strcmp0(profile, "performance") == 0)
        return "battery-full-symbolic";
    else if (g_strcmp0(profile, "balanced") == 0)
        return "battery-good-symbolic";
    else if (g_strcmp0(profile, "quiet") == 0 || g_strcmp0(profile, "low-power") == 0)
        return "battery-low-symbolic";
    else
        return "battery-good-symbolic";
}

static gboolean can_send_notification(AsusdBatteryPlugin *plugin) {
    if (!plugin) return FALSE;
    
    time_t current_time = time(NULL);
    if (current_time - plugin->last_notification_time < 2) {
        return FALSE;
    }
    
    plugin->last_notification_time = current_time;
    return TRUE;
}

static ProfileSettings* profile_settings_new(guint32 enum_value, const gchar *default_name) {
    ProfileSettings *settings = g_new0(ProfileSettings, 1);
    settings->enum_value = enum_value;
    settings->default_name = g_strdup(default_name ? default_name : "unknown");
    settings->name = NULL;
    settings->icon = NULL;
    return settings;
}

static void profile_settings_free(ProfileSettings *settings) {
    if (!settings) return;
    g_free(settings->name);
    g_free(settings->icon);
    g_free(settings->default_name);
    g_free(settings);
}

static const char* asusd_enum_to_default_name(guint32 enum_val) {
    switch (enum_val) {
        case 0: return "balanced";
        case 1: return "performance";
        case 2: return "quiet";
        default: return "unknown";
    }
}

static const gchar* profile_name_from_enum(AsusdBatteryPlugin *plugin, guint32 enum_value) {
    if (!plugin) return "unknown";
    
    ProfileSettings *settings = g_hash_table_lookup(plugin->profile_lookup, GUINT_TO_POINTER(enum_value));
    if (settings) {
        if (settings->name && *settings->name) {
            return settings->name;
        }
        if (settings->default_name && *settings->default_name) {
            return settings->default_name;
        }
    }
    return asusd_enum_to_default_name(enum_value);
}

static gboolean profile_enum_from_name(AsusdBatteryPlugin *plugin, const gchar *name, guint32 *enum_value) {
    if (!plugin || !name || !enum_value) return FALSE;
    
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, plugin->profile_lookup);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        ProfileSettings *settings = (ProfileSettings*)value;
        if (settings->name && g_strcmp0(name, settings->name) == 0) {
            *enum_value = settings->enum_value;
            return TRUE;
        }
        if (settings->default_name && g_strcmp0(name, settings->default_name) == 0) {
            *enum_value = settings->enum_value;
            return TRUE;
        }
    }
    return FALSE;
}

XFCE_PANEL_PLUGIN_REGISTER(asusd_battery_plugin_construct)

/* ========== ASUSD Функции ========== */

/* Универсальная функция установки свойства - без проверки AVAILABLE */
static gboolean asusd_set_property(AsusdBatteryPlugin *plugin, const char *property, GVariant *value) {
    if (!plugin || !plugin->dbus_connection || !property || !value) return FALSE;

    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        plugin->dbus_connection,
        ASUSD_BUS_NAME,
        ASUSD_OBJECT_PATH,
        DBUS_PROPERTIES_INTERFACE,
        "Set",
        g_variant_new("(ssv)", ASUSD_INTERFACE, property, value),
        G_VARIANT_TYPE("()"),
        G_DBUS_CALL_FLAGS_NONE,
        ASUSD_TIMEOUT_MS,
        NULL,
        &error
    );

    if (error) {
        g_warning("ASUSD: Failed to set %s: %s", property, error->message);
        g_error_free(error);
        return FALSE;
    }

    if (result) g_variant_unref(result);
    return TRUE;
}

/* Helper для получения свойства типа boolean - без проверки AVAILABLE */
static gboolean asusd_get_bool_property(AsusdBatteryPlugin *plugin, const char *property, gboolean *value) {
    if (!plugin || !plugin->dbus_connection || !property || !value) return FALSE;

    GError *error = NULL;
    GVariant *variant = g_dbus_connection_call_sync(
        plugin->dbus_connection,
        ASUSD_BUS_NAME,
        ASUSD_OBJECT_PATH,
        DBUS_PROPERTIES_INTERFACE,
        "Get",
        g_variant_new("(ss)", ASUSD_INTERFACE, property),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        ASUSD_TIMEOUT_MS,
        NULL,
        &error
    );

    if (error) {
        g_error_free(error);
        return FALSE;
    }

    if (!variant) return FALSE;

    GVariant *unwrapped = NULL;
    g_variant_get(variant, "(v)", &unwrapped);
    g_variant_unref(variant);

    if (!unwrapped) return FALSE;

    if (g_variant_is_of_type(unwrapped, G_VARIANT_TYPE_VARIANT)) {
        GVariant *inner = g_variant_get_variant(unwrapped);
        g_variant_unref(unwrapped);
        unwrapped = inner;
    }

    if (!g_variant_is_of_type(unwrapped, G_VARIANT_TYPE_BOOLEAN)) {
        g_variant_unref(unwrapped);
        return FALSE;
    }

    g_variant_get(unwrapped, "b", value);
    g_variant_unref(unwrapped);
    return TRUE;
}

/* Helper для получения свойства типа uint32 - без проверки AVAILABLE */
static gboolean asusd_get_uint32_property(AsusdBatteryPlugin *plugin, const char *property, guint32 *value) {
    if (!plugin || !plugin->dbus_connection || !property || !value) return FALSE;

    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        plugin->dbus_connection,
        ASUSD_BUS_NAME,
        ASUSD_OBJECT_PATH,
        DBUS_PROPERTIES_INTERFACE,
        "Get",
        g_variant_new("(ss)", ASUSD_INTERFACE, property),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        ASUSD_TIMEOUT_MS,
        NULL,
        &error
    );

    if (error) {
        g_debug("ASUSD: Failed to get %s: %s", property, error->message);
        g_error_free(error);
        return FALSE;
    }

    if (!result) return FALSE;

    GVariant *unwrapped = NULL;
    g_variant_get(result, "(v)", &unwrapped);
    g_variant_unref(result);

    if (!unwrapped) return FALSE;

    if (g_variant_is_of_type(unwrapped, G_VARIANT_TYPE_VARIANT)) {
        GVariant *inner = g_variant_get_variant(unwrapped);
        g_variant_unref(unwrapped);
        unwrapped = inner;
    }

    if (!g_variant_is_of_type(unwrapped, G_VARIANT_TYPE_UINT32)) {
        g_debug("ASUSD: %s is not uint32, type: %s", property, 
                g_variant_get_type_string(unwrapped));
        g_variant_unref(unwrapped);
        return FALSE;
    }

    g_variant_get(unwrapped, "u", value);
    g_variant_unref(unwrapped);
    
    g_debug("ASUSD: %s = %u", property, *value);
    return TRUE;
}

/* Helper для получения свойства типа byte - без проверки AVAILABLE */
static gboolean asusd_get_byte_property(AsusdBatteryPlugin *plugin, const char *property, guint8 *value) {
    if (!plugin || !plugin->dbus_connection || !property || !value) return FALSE;

    GError *error = NULL;
    GVariant *variant = g_dbus_connection_call_sync(
        plugin->dbus_connection,
        ASUSD_BUS_NAME,
        ASUSD_OBJECT_PATH,
        DBUS_PROPERTIES_INTERFACE,
        "Get",
        g_variant_new("(ss)", ASUSD_INTERFACE, property),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        ASUSD_TIMEOUT_MS,
        NULL,
        &error
    );

    if (error) {
        g_error_free(error);
        return FALSE;
    }

    if (!variant) return FALSE;

    GVariant *unwrapped = NULL;
    g_variant_get(variant, "(v)", &unwrapped);
    g_variant_unref(variant);

    if (!unwrapped) return FALSE;

    if (g_variant_is_of_type(unwrapped, G_VARIANT_TYPE_VARIANT)) {
        GVariant *inner = g_variant_get_variant(unwrapped);
        g_variant_unref(unwrapped);
        unwrapped = inner;
    }

    if (!g_variant_is_of_type(unwrapped, G_VARIANT_TYPE_BYTE)) {
        g_variant_unref(unwrapped);
        return FALSE;
    }

    g_variant_get(unwrapped, "y", value);
    g_variant_unref(unwrapped);
    return TRUE;
}

/* Загрузка поддерживаемых профилей - без проверки AVAILABLE */
static gboolean asusd_load_supported_profiles(AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->dbus_connection) return FALSE;

    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        plugin->dbus_connection,
        ASUSD_BUS_NAME,
        ASUSD_OBJECT_PATH,
        DBUS_PROPERTIES_INTERFACE,
        "Get",
        g_variant_new("(ss)", ASUSD_INTERFACE, "PlatformProfileChoices"),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        ASUSD_TIMEOUT_MS,
        NULL,
        &error
    );

    if (error) {
        g_warning("ASUSD: Failed to get PlatformProfileChoices: %s", error->message);
        g_error_free(error);
        return FALSE;
    }

    if (!result) {
        g_warning("ASUSD: No result for PlatformProfileChoices");
        return FALSE;
    }

    GVariant *value = NULL;
    g_variant_get(result, "(v)", &value);
    g_variant_unref(result);

    if (!value) {
        g_warning("ASUSD: No value in PlatformProfileChoices result");
        return FALSE;
    }

    if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
        GVariant *inner = g_variant_get_variant(value);
        g_variant_unref(value);
        value = inner;
    }

    if (!g_variant_is_of_type(value, G_VARIANT_TYPE_ARRAY)) {
        g_warning("ASUSD: PlatformProfileChoices is not an array, type: %s", 
                  g_variant_get_type_string(value));
        g_variant_unref(value);
        return FALSE;
    }

    /* Очищаем старые данные */
    if (plugin->profiles) {
        g_ptr_array_free(plugin->profiles, TRUE);
        plugin->profiles = NULL;
    }
    if (plugin->profile_lookup) {
        g_hash_table_destroy(plugin->profile_lookup);
        plugin->profile_lookup = NULL;
    }

    plugin->profiles = g_ptr_array_new_with_free_func((GDestroyNotify)profile_settings_free);
    plugin->profile_lookup = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);

    XfconfChannel *channel = xfconf_channel_get(CONFIG_CHANNEL);

    GVariantIter iter;
    guint32 enum_value;
    g_variant_iter_init(&iter, value);

    while (g_variant_iter_next(&iter, "u", &enum_value)) {
        const char *default_name = asusd_enum_to_default_name(enum_value);
        ProfileSettings *settings = profile_settings_new(enum_value, default_name);

        gchar *key = g_strdup_printf("%s/profile_%d_name", CONFIG_PROPERTY_PREFIX, enum_value);
        gchar *saved_name = xfconf_channel_get_string(channel, key, NULL);
        if (saved_name && strlen(saved_name) > 0) {
            settings->name = saved_name;
        } else {
            g_free(saved_name);
        }
        g_free(key);

        key = g_strdup_printf("%s/profile_%d_icon", CONFIG_PROPERTY_PREFIX, enum_value);
        gchar *saved_icon = xfconf_channel_get_string(channel, key, NULL);
        if (saved_icon && strlen(saved_icon) > 0) {
            settings->icon = saved_icon;
        } else {
            g_free(saved_icon);
        }
        g_free(key);

        g_ptr_array_add(plugin->profiles, settings);
        g_hash_table_insert(plugin->profile_lookup, GUINT_TO_POINTER(enum_value), settings);
        
        g_debug("ASUSD: Loaded profile: %s (enum: %u)", default_name, enum_value);
    }

    g_variant_unref(value);
    
    g_debug("ASUSD: Loaded %d profiles", plugin->profiles ? plugin->profiles->len : 0);
    return (plugin->profiles && plugin->profiles->len > 0);
}

/* Получение текущего профиля (только для инициализации) - без проверки AVAILABLE */
static gboolean asusd_get_current_profile_static(AsusdBatteryPlugin *plugin, gchar **profile_name) {
    if (!plugin || !plugin->dbus_connection || !profile_name) return FALSE;

    guint32 enum_value;
    if (!asusd_get_uint32_property(plugin, "PlatformProfile", &enum_value)) {
        *profile_name = g_strdup("unknown");
        return FALSE;
    }

    const gchar *name = profile_name_from_enum(plugin, enum_value);
    *profile_name = g_strdup(name);
    return TRUE;
}

/* Установка профиля - С проверкой AVAILABLE (пользовательская операция) */
static gboolean asusd_set_profile(AsusdBatteryPlugin *plugin, const gchar *profile_name) {
    if (!plugin || !profile_name) return FALSE;
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) return FALSE;

    guint32 enum_val = 999;
    if (!profile_enum_from_name(plugin, profile_name, &enum_val)) {
        g_warning("ASUSD: Profile %s not found", profile_name);
        return FALSE;
    }

    return asusd_set_property(plugin, "PlatformProfile", g_variant_new_uint32(enum_val));
}

/* Получение списка доступных профилей (только для меню) */
static gchar** asusd_get_available_profiles(AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->profiles) return NULL;

    GPtrArray *result = g_ptr_array_new();
    
    for (guint i = 0; i < plugin->profiles->len; i++) {
        ProfileSettings *settings = g_ptr_array_index(plugin->profiles, i);
        const char *display_name = NULL;
        
        if (settings->name && strlen(settings->name) > 0) {
            display_name = settings->name;
        } else {
            display_name = settings->default_name;
        }
        
        if (display_name) {
            g_ptr_array_add(result, g_strdup(display_name));
        }
    }

    g_ptr_array_add(result, NULL);
    return (gchar**)g_ptr_array_free(result, FALSE);
}

/* Обработка изменения свойств ASUSD */
static void asusd_handle_properties_changed(AsusdBatteryPlugin *plugin, GVariant *changed_properties) {
    if (!plugin || !changed_properties) return;

    if (plugin->saving_settings) {
        g_debug("asusd_handle_properties_changed: IGNORED (saving_settings)");
        return;
    }

    GVariant *profile_variant = g_variant_lookup_value(changed_properties, 
                                                      "PlatformProfile", 
                                                      G_VARIANT_TYPE_UINT32);
    if (profile_variant) {
        guint32 enum_value;
        g_variant_get(profile_variant, "u", &enum_value);
        g_variant_unref(profile_variant);

        const gchar *name = profile_name_from_enum(plugin, enum_value);

        if (plugin->current_profile && g_strcmp0(plugin->current_profile, name) == 0) {
            g_debug("  >>> Profile did not actually change (already %s), ignoring signal", name);
            return;
        }

        g_debug("  PlatformProfile changed to %u (%s)", enum_value, name);

        g_free(plugin->current_profile);
        plugin->current_profile = g_strdup(name);
        
        update_profile_display(plugin, TRUE);
    }

    /* ChargeControlEndThreshold */
    GVariant *limit_variant = g_variant_lookup_value(changed_properties, 
                                                    "ChargeControlEndThreshold", 
                                                    G_VARIANT_TYPE_BYTE);
    if (limit_variant) {
        guint8 limit;
        g_variant_get(limit_variant, "y", &limit);
        g_variant_unref(limit_variant);

        plugin->current_battery_limit = limit;
        plugin->battery_limit_enabled = (limit == 80);
    } else {
        GVariant *limit_variant_u32 = g_variant_lookup_value(changed_properties, 
                                                            "ChargeControlEndThreshold", 
                                                            G_VARIANT_TYPE_UINT32);
        if (limit_variant_u32) {
            guint32 limit;
            g_variant_get(limit_variant_u32, "u", &limit);
            g_variant_unref(limit_variant_u32);

            plugin->current_battery_limit = limit;
            plugin->battery_limit_enabled = (limit == 80);
        }
    }

    /* Auto-switch настройки */
    GVariant *ac_enabled = g_variant_lookup_value(changed_properties, 
                                                 "ChangePlatformProfileOnAc", 
                                                 G_VARIANT_TYPE_BOOLEAN);
    if (ac_enabled) {
        g_variant_get(ac_enabled, "b", &plugin->auto_switch_ac_enabled);
        g_variant_unref(ac_enabled);
    }

    GVariant *bat_enabled = g_variant_lookup_value(changed_properties, 
                                                  "ChangePlatformProfileOnBattery", 
                                                  G_VARIANT_TYPE_BOOLEAN);
    if (bat_enabled) {
        g_variant_get(bat_enabled, "b", &plugin->auto_switch_battery_enabled);
        g_variant_unref(bat_enabled);
    }

    GVariant *ac_profile = g_variant_lookup_value(changed_properties, 
                                                 "PlatformProfileOnAc", 
                                                 G_VARIANT_TYPE_UINT32);
    if (ac_profile) {
        guint32 enum_val;
        g_variant_get(ac_profile, "u", &enum_val);
        g_variant_unref(ac_profile);

        const gchar *name = profile_name_from_enum(plugin, enum_val);
        g_free(plugin->auto_switch_ac_profile);
        plugin->auto_switch_ac_profile = g_strdup(name);
    }

    GVariant *bat_profile = g_variant_lookup_value(changed_properties, 
                                                  "PlatformProfileOnBattery", 
                                                  G_VARIANT_TYPE_UINT32);
    if (bat_profile) {
        guint32 enum_val;
        g_variant_get(bat_profile, "u", &enum_val);
        g_variant_unref(bat_profile);

        const gchar *name = profile_name_from_enum(plugin, enum_val);
        g_free(plugin->auto_switch_battery_profile);
        plugin->auto_switch_battery_profile = g_strdup(name);
    }
}

/* Обработка NameOwnerChanged */
static void asusd_handle_name_owner_changed(AsusdBatteryPlugin *plugin,
                                            const gchar *old_owner,
                                            const gchar *new_owner) {
    (void)old_owner;

    if (!plugin) return;

    if (new_owner && *new_owner) {
        g_debug("ASUSD: service appeared, owner=%s", new_owner);

        if (plugin->asusd_retry_timeout_id > 0) {
            g_source_remove(plugin->asusd_retry_timeout_id);
            plugin->asusd_retry_timeout_id = 0;
        }

        plugin->asusd_state = ASUSD_STATE_CONNECTING;
        if (!asusd_init(plugin)) {
            g_warning("ASUSD: service is present but initial state synchronization failed");
        }
    } else {
        g_debug("ASUSD: service disappeared");
        reset_state(plugin);

        if (plugin->asusd_retry_timeout_id == 0) {
            plugin->asusd_retry_timeout_id = g_timeout_add_seconds(5, asusd_retry_init, plugin);
        }
    }
}

/* Retry только как fallback. NameOwnerChanged остается основным механизмом. */
static gboolean asusd_retry_init(gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin *)user_data;
    if (!plugin) return G_SOURCE_REMOVE;

    plugin->asusd_retry_timeout_id = 0;

    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) {
        plugin->asusd_state = ASUSD_STATE_CONNECTING;
        if (!asusd_init(plugin)) {
            if (plugin->asusd_retry_timeout_id == 0) {
                plugin->asusd_retry_timeout_id = g_timeout_add_seconds(5, asusd_retry_init, plugin);
            }
        }
    }

    return G_SOURCE_REMOVE;
}

/* Установка подписок D-Bus */
static gboolean asusd_setup_monitoring(AsusdBatteryPlugin *plugin) {
    if (!plugin) return FALSE;

    if (!plugin->dbus_connection) {
        GError *error = NULL;
        plugin->dbus_connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
        if (!plugin->dbus_connection) {
            g_warning("ASUSD: Failed to connect to system bus: %s",
                      error ? error->message : "unknown error");
            g_clear_error(&error);
            return FALSE;
        }
    }

    if (plugin->asusd_properties_signal_id == 0) {
        plugin->asusd_properties_signal_id = g_dbus_connection_signal_subscribe(
            plugin->dbus_connection,
            ASUSD_BUS_NAME,
            DBUS_PROPERTIES_INTERFACE,
            "PropertiesChanged",
            ASUSD_OBJECT_PATH,
            ASUSD_INTERFACE,
            G_DBUS_SIGNAL_FLAGS_NONE,
            (GDBusSignalCallback)on_dbus_signal,
            plugin,
            NULL
        );

        if (plugin->asusd_properties_signal_id == 0) {
            g_warning("ASUSD: Failed to subscribe to PropertiesChanged");
            return FALSE;
        }
    }

    if (plugin->asusd_name_owner_signal_id == 0) {
        plugin->asusd_name_owner_signal_id = g_dbus_connection_signal_subscribe(
            plugin->dbus_connection,
            "org.freedesktop.DBus",
            "org.freedesktop.DBus",
            DBUS_NAME_OWNER_CHANGED,
            "/org/freedesktop/DBus",
            ASUSD_BUS_NAME,
            G_DBUS_SIGNAL_FLAGS_NONE,
            (GDBusSignalCallback)on_dbus_signal,
            plugin,
            NULL
        );

        if (plugin->asusd_name_owner_signal_id == 0) {
            g_warning("ASUSD: Failed to subscribe to NameOwnerChanged");
            return FALSE;
        }
    }

    return TRUE;
}

/* Основная инициализация ASUSD - подписки создаются до синхронизации */
static gboolean asusd_init(AsusdBatteryPlugin *plugin) {
    if (!plugin) return FALSE;

    /* 1. Устанавливаем подписки (создают соединение при необходимости) */
    if (!asusd_setup_monitoring(plugin)) {
        plugin->asusd_state = ASUSD_STATE_UNAVAILABLE;
        return FALSE;
    }

    /* 2. Проверяем, что asusd владеет well-known name */
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        plugin->dbus_connection,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "GetNameOwner",
        g_variant_new("(s)", ASUSD_BUS_NAME),
        G_VARIANT_TYPE("(s)"),
        G_DBUS_CALL_FLAGS_NONE,
        ASUSD_TIMEOUT_MS,
        NULL,
        &error
    );

    if (!result) {
        g_debug("ASUSD: GetNameOwner: %s",
                error ? error->message : "no owner");
        g_clear_error(&error);
        plugin->asusd_state = ASUSD_STATE_UNAVAILABLE;
        return FALSE;
    }

    gchar *owner = NULL;
    g_variant_get(result, "(s)", &owner);
    g_variant_unref(result);

    g_debug("ASUSD: found service owner %s", owner ? owner : "<none>");
    g_free(owner);

    /* 3. Синхронизируем состояние (функции не проверяют AVAILABLE) */
    plugin->asusd_state = ASUSD_STATE_CONNECTING;

    if (!asusd_load_supported_profiles(plugin)) {
        g_warning("ASUSD: failed to synchronize PlatformProfileChoices");
        plugin->asusd_state = ASUSD_STATE_UNAVAILABLE;
        return FALSE;
    }

    gchar *profile_name = NULL;
    if (!asusd_get_current_profile_static(plugin, &profile_name)) {
        g_warning("ASUSD: failed to synchronize PlatformProfile");
        plugin->asusd_state = ASUSD_STATE_UNAVAILABLE;
        return FALSE;
    }

    g_free(plugin->current_profile);
    plugin->current_profile = profile_name;

    guint8 limit;
    if (asusd_get_byte_property(plugin, "ChargeControlEndThreshold", &limit)) {
        plugin->current_battery_limit = limit;
        plugin->battery_limit_enabled = (limit == 80);
    }

    asusd_get_bool_property(plugin, "ChangePlatformProfileOnAc", &plugin->auto_switch_ac_enabled);
    asusd_get_bool_property(plugin, "ChangePlatformProfileOnBattery", &plugin->auto_switch_battery_enabled);

    guint32 enum_val;
    if (asusd_get_uint32_property(plugin, "PlatformProfileOnAc", &enum_val)) {
        const gchar *name = profile_name_from_enum(plugin, enum_val);
        g_free(plugin->auto_switch_ac_profile);
        plugin->auto_switch_ac_profile = g_strdup(name);
    }

    if (asusd_get_uint32_property(plugin, "PlatformProfileOnBattery", &enum_val)) {
        const gchar *name = profile_name_from_enum(plugin, enum_val);
        g_free(plugin->auto_switch_battery_profile);
        plugin->auto_switch_battery_profile = g_strdup(name);
    }

    plugin->asusd_state = ASUSD_STATE_AVAILABLE;
    update_profile_display(plugin, FALSE);
    g_debug("ASUSD: initialization/synchronization completed successfully");

    return TRUE;
}

/* Очистка ASUSD */
static void asusd_cleanup(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;

    if (plugin->asusd_properties_signal_id > 0 && plugin->dbus_connection) {
        g_dbus_connection_signal_unsubscribe(plugin->dbus_connection,
                                             plugin->asusd_properties_signal_id);
        plugin->asusd_properties_signal_id = 0;
    }

    if (plugin->asusd_name_owner_signal_id > 0 && plugin->dbus_connection) {
        g_dbus_connection_signal_unsubscribe(plugin->dbus_connection,
                                             plugin->asusd_name_owner_signal_id);
        plugin->asusd_name_owner_signal_id = 0;
    }

    if (plugin->upower_properties_signal_id > 0 && plugin->dbus_connection) {
        g_dbus_connection_signal_unsubscribe(plugin->dbus_connection,
                                             plugin->upower_properties_signal_id);
        plugin->upower_properties_signal_id = 0;
    }

    if (plugin->profiles) {
        g_ptr_array_free(plugin->profiles, TRUE);
        plugin->profiles = NULL;
    }

    if (plugin->profile_lookup) {
        g_hash_table_destroy(plugin->profile_lookup);
        plugin->profile_lookup = NULL;
    }

    if (plugin->asusd_retry_timeout_id > 0) {
        g_source_remove(plugin->asusd_retry_timeout_id);
        plugin->asusd_retry_timeout_id = 0;
    }

    if (plugin->dbus_connection) {
        g_object_unref(plugin->dbus_connection);
        plugin->dbus_connection = NULL;
    }

    plugin->asusd_state = ASUSD_STATE_UNAVAILABLE;
}

/* ========== Основные функции плагина ========== */

static void init_i18n(void) {
    bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);
}

static void asusd_battery_plugin_construct(XfcePanelPlugin *plugin) {
    AsusdBatteryPlugin *plugin_data;
    GError *error = NULL;

    init_i18n();

    if (!xfconf_init(&error)) {
        g_warning("Failed to initialize xfconf: %s", error ? error->message : "unknown");
        if (error) g_error_free(error);
        return;
    }

    plugin_data = g_new0(AsusdBatteryPlugin, 1);
    if (!plugin_data) {
        g_warning("Failed to allocate memory for plugin");
        return;
    }

    plugin_data->plugin = plugin;
    plugin_data->current_profile = g_strdup("unknown");
    plugin_data->asusd_state = ASUSD_STATE_UNAVAILABLE;
    plugin_data->hide_icon = FALSE;
    plugin_data->hide_text = FALSE;
    plugin_data->hide_notifications = FALSE;
    plugin_data->is_on_ac = TRUE;
    plugin_data->last_notification_time = 0;
    plugin_data->settings_dialog_open = FALSE;
    plugin_data->saving_settings = FALSE;

    load_settings(plugin_data);

    /* Создаем кнопку */
    plugin_data->button = gtk_button_new();
    if (!plugin_data->button) {
        g_warning("Failed to create button");
        g_free(plugin_data);
        return;
    }

    gtk_button_set_relief(GTK_BUTTON(plugin_data->button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(plugin_data->button, FALSE);
    gtk_widget_set_tooltip_text(plugin_data->button, _("Manage performance profile"));

    plugin_data->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_add(GTK_CONTAINER(plugin_data->button), plugin_data->box);

    plugin_data->image = gtk_image_new_from_icon_name("battery-good-symbolic", GTK_ICON_SIZE_MENU);
    gtk_box_pack_start(GTK_BOX(plugin_data->box), plugin_data->image, FALSE, FALSE, 0);

    plugin_data->label = gtk_label_new(NULL);
    gtk_box_pack_start(GTK_BOX(plugin_data->box), plugin_data->label, FALSE, FALSE, 0);

    if (!asusd_init(plugin_data)) {
        g_debug("ASUSD: service is currently unavailable; waiting for NameOwnerChanged");
    }

    update_profile_display(plugin_data, FALSE);

    g_signal_connect(G_OBJECT(plugin_data->button), "clicked", G_CALLBACK(on_button_clicked), plugin_data);
    g_signal_connect(G_OBJECT(plugin_data->button), "button-press-event", G_CALLBACK(on_button_press), plugin_data);

    gtk_container_add(GTK_CONTAINER(plugin), plugin_data->button);
    gtk_widget_show_all(plugin_data->button);

    setup_upower_monitoring(plugin_data);

    g_object_set_data_full(G_OBJECT(plugin), "plugin_data", plugin_data, asusd_battery_plugin_free);
}

static void asusd_battery_plugin_free(gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin *)user_data;
    if (!plugin) return;

    asusd_cleanup(plugin);

    g_free(plugin->current_profile);
    g_free(plugin->auto_switch_ac_profile);
    g_free(plugin->auto_switch_battery_profile);
    g_free(plugin->last_displayed_profile);

    g_free(plugin);
}

/* Отправка уведомления через spawn */
static void send_notification(const gchar *message, const gchar *subtitle, gboolean is_error, const gchar *icon) {
    if (!message) return;

    const gchar *icon_name = icon ? icon : (is_error ? "dialog-error" : "battery-good-symbolic");
    const gchar *urgency = is_error ? "critical" : "normal";

    gchar *argv[] = {
        (gchar*)"notify-send",
        (gchar*)"--urgency", (gchar*)urgency,
        (gchar*)"--icon", (gchar*)icon_name,
        (gchar*)message,
        (gchar*)(subtitle ? subtitle : ""),
        NULL
    };

    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

static void load_settings(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;

    XfconfChannel *channel = xfconf_channel_get(CONFIG_CHANNEL);
    if (!channel) {
        g_warning("Failed to get xfconf channel");
        return;
    }

    plugin->hide_icon = xfconf_channel_get_bool(channel, CONFIG_PROPERTY_PREFIX "/hide_icon", FALSE);
    plugin->hide_text = xfconf_channel_get_bool(channel, CONFIG_PROPERTY_PREFIX "/hide_text", FALSE);
    plugin->hide_notifications = xfconf_channel_get_bool(channel, CONFIG_PROPERTY_PREFIX "/hide_notifications", FALSE);
}

static void save_settings(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;

    XfconfChannel *channel = xfconf_channel_get(CONFIG_CHANNEL);
    if (!channel) {
        g_warning("Failed to get xfconf channel");
        return;
    }

    xfconf_channel_set_bool(channel, CONFIG_PROPERTY_PREFIX "/hide_icon", plugin->hide_icon);
    xfconf_channel_set_bool(channel, CONFIG_PROPERTY_PREFIX "/hide_text", plugin->hide_text);
    xfconf_channel_set_bool(channel, CONFIG_PROPERTY_PREFIX "/hide_notifications", plugin->hide_notifications);

    if (plugin->profiles) {
        for (guint i = 0; i < plugin->profiles->len; i++) {
            ProfileSettings *settings = g_ptr_array_index(plugin->profiles, i);
            
            gchar *key = g_strdup_printf("%s/profile_%d_name", CONFIG_PROPERTY_PREFIX, settings->enum_value);
            if (settings->name && strlen(settings->name) > 0) {
                xfconf_channel_set_string(channel, key, settings->name);
            } else {
                xfconf_channel_set_string(channel, key, "");
            }
            g_free(key);

            key = g_strdup_printf("%s/profile_%d_icon", CONFIG_PROPERTY_PREFIX, settings->enum_value);
            if (settings->icon && strlen(settings->icon) > 0) {
                xfconf_channel_set_string(channel, key, settings->icon);
            } else {
                xfconf_channel_set_string(channel, key, "");
            }
            g_free(key);
        }
    }
}

/* UPower мониторинг */
static void setup_upower_monitoring(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;

    if (!plugin->dbus_connection) {
        GError *error = NULL;
        plugin->dbus_connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
        if (!plugin->dbus_connection) {
            g_warning("Failed to connect to system bus: %s", error ? error->message : "unknown");
            if (error) g_error_free(error);
            return;
        }
    }

    if (plugin->upower_properties_signal_id == 0) {
        plugin->upower_properties_signal_id = g_dbus_connection_signal_subscribe(
            plugin->dbus_connection,
            "org.freedesktop.UPower",
            "org.freedesktop.DBus.Properties",
            "PropertiesChanged",
            "/org/freedesktop/UPower",
            "org.freedesktop.UPower",
            G_DBUS_SIGNAL_FLAGS_NONE,
            (GDBusSignalCallback)on_dbus_signal,
            plugin,
            NULL
        );
    }
}

/* D-Bus сигналы - исправленная версия */
static void on_dbus_signal(GDBusConnection *connection, const gchar *sender_name,
                           const gchar *object_path, const gchar *interface_name,
                           const gchar *signal_name, GVariant *parameters,
                           AsusdBatteryPlugin *plugin) {
    if (!plugin || !parameters) return;

    if (plugin->saving_settings) {
        g_debug("=== on_dbus_signal: IGNORED (saving_settings) ===");
        return;
    }

    /* ASUSD PropertiesChanged - проверяем object_path, а не sender_name */
    if (g_strcmp0(signal_name, "PropertiesChanged") == 0 &&
        g_strcmp0(object_path, ASUSD_OBJECT_PATH) == 0) {

        const char *changed_interface = NULL;
        GVariant *changed_properties = NULL;
        GVariant *invalidated_properties = NULL;

        /* ПРАВИЛЬНЫЙ формат: (&s@a{sv}@as) */
        g_variant_get(parameters, "(&s@a{sv}@as)",
                      &changed_interface, &changed_properties, &invalidated_properties);

        g_debug("=== on_dbus_signal: ASUSD PropertiesChanged ===");
        g_debug("  changed_interface = %s", changed_interface ? changed_interface : "NULL");
        g_debug("  asusd_state = %d", plugin->asusd_state);

        if (changed_properties) {
            GVariantIter iter;
            gchar *key;
            GVariant *value;
            g_variant_iter_init(&iter, changed_properties);
            while (g_variant_iter_next(&iter, "{sv}", &key, &value)) {
                g_debug("    changed property: %s = %s", key, g_variant_get_type_string(value));
                g_free(key);
                g_variant_unref(value);
            }
        }

        if (g_strcmp0(changed_interface, ASUSD_INTERFACE) == 0 &&
            plugin->asusd_state == ASUSD_STATE_AVAILABLE) {
            g_debug("  >>> Calling asusd_handle_properties_changed");
            asusd_handle_properties_changed(plugin, changed_properties);
        } else {
            g_debug("  >>> SKIPPING: interface=%s, state=%d", 
                    changed_interface ? changed_interface : "NULL", 
                    plugin->asusd_state);
        }

        if (changed_properties) g_variant_unref(changed_properties);
        if (invalidated_properties) g_variant_unref(invalidated_properties);
        return;
    }

    /* NameOwnerChanged - уже отфильтровано subscription-ом */
    if (g_strcmp0(signal_name, DBUS_NAME_OWNER_CHANGED) == 0 &&
        g_strcmp0(object_path, "/org/freedesktop/DBus") == 0) {

        const char *name, *old_owner, *new_owner;
        g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);

        g_debug("=== on_dbus_signal: NameOwnerChanged ===");
        g_debug("  name = %s, old = %s, new = %s", 
                name, old_owner ? old_owner : "NULL", new_owner ? new_owner : "NULL");

        if (g_strcmp0(name, ASUSD_BUS_NAME) == 0) {
            asusd_handle_name_owner_changed(plugin, old_owner, new_owner);
        }
        return;
    }

    /* UPower PropertiesChanged */
    if (g_strcmp0(signal_name, "PropertiesChanged") == 0) {
        const gchar *interface = NULL;
        GVariant *changed_properties = NULL;
        GVariant *invalidated_properties = NULL;
        gboolean on_battery = FALSE;
        gboolean status_changed = FALSE;

        g_variant_get(parameters, "(&s@a{sv}@as)", 
                      &interface, &changed_properties, &invalidated_properties);

        if (g_strcmp0(interface, "org.freedesktop.UPower") == 0 && changed_properties) {
            GVariantDict dict;
            g_variant_dict_init(&dict, changed_properties);
            GVariant *value = g_variant_dict_lookup_value(&dict, "OnBattery", NULL);
            
            if (value) {
                on_battery = g_variant_get_boolean(value);
                status_changed = TRUE;
                g_debug("=== on_dbus_signal: UPower OnBattery = %d ===", on_battery);
                g_variant_unref(value);
            }
            
            g_variant_dict_clear(&dict);
        }

        if (changed_properties) g_variant_unref(changed_properties);
        if (invalidated_properties) g_variant_unref(invalidated_properties);

        if (status_changed) {
            plugin->is_on_ac = !on_battery;
        }
    }
}

/* Обновление отображения (только GTK, без D-Bus) */
static void update_profile_display(AsusdBatteryPlugin *plugin, gboolean should_notify) {
    if (!plugin) return;

    const gchar *profile = plugin->current_profile ? plugin->current_profile : "unknown";

    g_debug("=== update_profile_display ===");
    g_debug("  should_notify = %d", should_notify);
    g_debug("  hide_notifications = %d", plugin->hide_notifications);
    g_debug("  profile = '%s'", profile);
    g_debug("  last_displayed_profile = '%s'", 
            plugin->last_displayed_profile ? plugin->last_displayed_profile : "NULL");

    gboolean profile_changed = FALSE;
    
    if (should_notify && !plugin->hide_notifications && plugin->asusd_state == ASUSD_STATE_AVAILABLE) {
        if (plugin->last_displayed_profile == NULL || 
            g_strcmp0(plugin->last_displayed_profile, profile) != 0) {
            profile_changed = TRUE;
            g_debug("  >>> profile_changed = TRUE");
        } else {
            g_debug("  >>> profile_changed = FALSE");
        }
    } else {
        if (plugin->hide_notifications) {
            g_debug("  >>> Notifications are disabled by user (checkbox is ON)");
        }
        g_debug("  >>> profile_changed = FALSE");
    }

    g_free(plugin->last_displayed_profile);
    plugin->last_displayed_profile = g_strdup(profile);
    g_debug("  updated last_displayed_profile = '%s'", plugin->last_displayed_profile);

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

    if (profile && g_strcmp0(profile, "unknown") != 0) {
        const gchar *icon_name = NULL;
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, plugin->profile_lookup);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            ProfileSettings *settings = (ProfileSettings*)value;
            if (settings->name && g_strcmp0(profile, settings->name) == 0) {
                if (settings->icon && strlen(settings->icon) > 0) {
                    icon_name = settings->icon;
                }
                break;
            }
            if (settings->default_name && g_strcmp0(profile, settings->default_name) == 0) {
                if (settings->icon && strlen(settings->icon) > 0) {
                    icon_name = settings->icon;
                }
                break;
            }
        }

        if (!plugin->hide_text) {
            gchar *display_text = g_strdup(profile);
            if (g_strcmp0(profile, "balanced") == 0 ||
                g_strcmp0(profile, "performance") == 0 ||
                g_strcmp0(profile, "quiet") == 0) {
                if (strlen(display_text) > 0) {
                    display_text[0] = g_ascii_toupper(display_text[0]);
                }
            }
            gtk_label_set_text(GTK_LABEL(plugin->label), display_text);
            g_free(display_text);
        } else {
            gtk_label_set_text(GTK_LABEL(plugin->label), "");
        }

        if (icon_name) {
            gtk_image_set_from_icon_name(GTK_IMAGE(plugin->image), icon_name, GTK_ICON_SIZE_MENU);
        } else {
            const gchar *icon = get_profile_icon(plugin, profile);
            gtk_image_set_from_icon_name(GTK_IMAGE(plugin->image), icon, GTK_ICON_SIZE_MENU);
        }
    } else {
        gtk_label_set_text(GTK_LABEL(plugin->label), "?");
        gtk_image_set_from_icon_name(GTK_IMAGE(plugin->image), "battery-missing-symbolic", GTK_ICON_SIZE_MENU);
    }

    if (profile_changed && profile && g_strcmp0(profile, "unknown") != 0 && can_send_notification(plugin)) {
        g_debug("  >>> SENDING NOTIFICATION for profile: %s", profile);
        gchar *display_name = g_strdup(profile);
        if (display_name[0] >= 'a' && display_name[0] <= 'z') {
            display_name[0] = g_ascii_toupper(display_name[0]);
        }
        const gchar *icon = get_profile_icon(plugin, profile);
        gchar *subtitle = g_strdup_printf(_("Current profile: %s"), display_name);
        send_notification(_("Performance profile changed"), subtitle, FALSE, icon);
        g_free(subtitle);
        g_free(display_name);
    }
    g_debug("=== end update_profile_display ===");
}

/* Клик по кнопке */
static void on_button_clicked(GtkWidget *widget, AsusdBatteryPlugin *plugin) {
    if (!plugin || !widget) return;
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) return;

    GtkWidget *menu = gtk_menu_new();
    if (!menu) return;

    gchar **profiles = asusd_get_available_profiles(plugin);
    const gchar *current = plugin->current_profile ? plugin->current_profile : "unknown";

    if (profiles) {
        for (int i = 0; profiles[i] != NULL; i++) {
            gchar *icon_name = NULL;
            GHashTableIter iter;
            gpointer key, value;
            g_hash_table_iter_init(&iter, plugin->profile_lookup);
            while (g_hash_table_iter_next(&iter, &key, &value)) {
                ProfileSettings *settings = (ProfileSettings*)value;
                if (settings->name && g_strcmp0(profiles[i], settings->name) == 0) {
                    if (settings->icon && strlen(settings->icon) > 0) {
                        icon_name = g_strdup(settings->icon);
                    }
                    break;
                }
                if (settings->default_name && g_strcmp0(profiles[i], settings->default_name) == 0) {
                    if (settings->icon && strlen(settings->icon) > 0) {
                        icon_name = g_strdup(settings->icon);
                    }
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

/* Выбор профиля из меню */
static void on_profile_selected(GtkMenuItem *item, AsusdBatteryPlugin *plugin) {
    if (!item || !plugin) return;
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) return;

    gchar *profile = (gchar *)g_object_get_data(G_OBJECT(item), "profile");
    if (!profile) return;

    if (asusd_set_profile(plugin, profile)) {
        guint32 enum_val;
        if (asusd_get_uint32_property(plugin, "PlatformProfile", &enum_val)) {
            const gchar *name = profile_name_from_enum(plugin, enum_val);
            g_free(plugin->current_profile);
            plugin->current_profile = g_strdup(name);
        }
        update_profile_display(plugin, TRUE);
    } else {
        if (!plugin->hide_notifications) {
            send_notification(_("Error changing profile"),
                             _("Failed to set profile via ASUSD"),
                             TRUE, "emblem-readonly");
        }
    }
}

/* Правая кнопка мыши */
static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, AsusdBatteryPlugin *plugin) {
    if (!plugin || !widget) return FALSE;

    if (event->button == 3) {
        GtkWidget *menu = gtk_menu_new();
        if (!menu) return FALSE;

        GtkWidget *item = gtk_menu_item_new_with_label(_("Settings"));
        if (item) {
            g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(on_menu_configure), plugin);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        }

        item = gtk_separator_menu_item_new();
        if (item) {
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        }

        item = gtk_menu_item_new_with_label(_("About"));
        if (item) {
            g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(on_menu_about), plugin);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        }

        gtk_widget_show_all(menu);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent*)event);
        return TRUE;
    }
    return FALSE;
}

static void on_menu_configure(GtkMenuItem *item, AsusdBatteryPlugin *plugin) {
    if (!item || !plugin) return;
    create_settings_dialog(plugin);
}

static void on_menu_about(GtkMenuItem *item, AsusdBatteryPlugin *plugin) {
    if (!item || !plugin) return;
    create_about_dialog(plugin);
}

/* ========== Диалоги ========== */

static void on_dialog_destroy(GtkWidget *widget, AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    
    if (plugin->settings_dialog_open) {
        g_debug("on_dialog_destroy: settings dialog closed, resetting flags");
        plugin->settings_dialog_open = FALSE;
        plugin->saving_settings = FALSE;
    }
}

static void create_about_dialog(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;

    GtkWidget *dialog = gtk_dialog_new_with_buttons(_("About ASUS Battery Plugin"),
                                        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(plugin->plugin))),
                                        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                        NULL, NULL);

    if (!dialog) return;

    gtk_window_set_icon_name(GTK_WINDOW(dialog), "dialog-information");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 400, 280);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    if (!content_area) {
        gtk_widget_destroy(dialog);
        return;
    }

    gtk_container_set_border_width(GTK_CONTAINER(content_area), 20);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(content_area), vbox);

    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), _("<b>ASUS Battery</b>"));
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

    label = gtk_label_new(_("Version 1.0"));
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), separator, FALSE, FALSE, 5);

    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), 
                         _("<small>Plugin for managing ASUS laptop power profiles via asusd.\n"
                           "Allows switching between performance, balanced and quiet modes.</small>"));
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label),
                         _("<small><b>Authors:</b> Deepseek and korn3r</small>"));
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);

    GtkWidget *close_button = gtk_button_new_with_label(_("Close"));
    if (close_button) {
        g_signal_connect_swapped(G_OBJECT(close_button), "clicked", 
                                 G_CALLBACK(gtk_widget_destroy), dialog);
        gtk_box_pack_start(GTK_BOX(vbox), close_button, FALSE, FALSE, 10);
        gtk_widget_set_halign(close_button, GTK_ALIGN_CENTER);
    }

    gtk_widget_show_all(dialog);
}

/* Обработчик для кнопки Close в настройках */
static void on_close_button_clicked(GtkButton *button, GtkWidget *dialog) {
    g_debug("=== on_close_button_clicked: closing dialog without saving ===");
    gtk_widget_destroy(dialog);
}

/* Обработчик изменения любого элемента в диалоге (кроме Hide icon/text) */
static void on_any_setting_changed(GtkWidget *widget, AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    g_debug("on_any_setting_changed: setting changed");
}

/* Обработчик для Hide icon и Hide text - правильная логика взаимоисключения */
static void on_hide_toggle(GtkToggleButton *toggle_button, AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    
    GtkWidget *dialog = GTK_WIDGET(gtk_widget_get_toplevel(GTK_WIDGET(toggle_button)));
    if (!dialog) return;
    
    GtkWidget *hide_icon_check = g_object_get_data(G_OBJECT(dialog), "hide_icon_check");
    GtkWidget *hide_text_check = g_object_get_data(G_OBJECT(dialog), "hide_text_check");
    
    gboolean icon_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(hide_icon_check));
    gboolean text_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(hide_text_check));
    
    if (icon_active && text_active) {
        if (toggle_button == GTK_TOGGLE_BUTTON(hide_icon_check)) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hide_text_check), FALSE);
            g_debug("on_hide_toggle: Icon enabled, disabling Text");
        } else {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hide_icon_check), FALSE);
            g_debug("on_hide_toggle: Text enabled, disabling Icon");
        }
    }
}

/* Обработчик кнопки "Применить" */
static void on_apply_clicked(GtkButton *button, AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->settings_dialog_open) return;
    
    g_debug("=== on_apply_clicked: Apply button clicked ===");
    
    GtkWidget *dialog = GTK_WIDGET(gtk_widget_get_toplevel(GTK_WIDGET(button)));
    if (!dialog) return;
    
    GtkWidget *check_ac = g_object_get_data(G_OBJECT(dialog), "check_ac");
    GtkWidget *check_battery = g_object_get_data(G_OBJECT(dialog), "check_battery");
    GtkWidget *combo_ac = g_object_get_data(G_OBJECT(dialog), "combo_ac");
    GtkWidget *combo_battery = g_object_get_data(G_OBJECT(dialog), "combo_battery");
    GtkWidget *limit_check = g_object_get_data(G_OBJECT(dialog), "limit_check");
    GtkWidget *hide_icon_check = g_object_get_data(G_OBJECT(dialog), "hide_icon_check");
    GtkWidget *hide_text_check = g_object_get_data(G_OBJECT(dialog), "hide_text_check");
    GtkWidget *notifications_check = g_object_get_data(G_OBJECT(dialog), "notifications_check");
    
    /* ЛОКАЛЬНЫЕ НАСТРОЙКИ (применяются всегда) */
    gboolean new_hide_icon = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(hide_icon_check));
    gboolean new_hide_text = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(hide_text_check));
    gboolean new_hide_notifications = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(notifications_check));
    
    gboolean hide_changed = FALSE;
    if (new_hide_icon != plugin->hide_icon) {
        g_debug("  hide_icon changed: %d -> %d", plugin->hide_icon, new_hide_icon);
        plugin->hide_icon = new_hide_icon;
        hide_changed = TRUE;
    }
    if (new_hide_text != plugin->hide_text) {
        g_debug("  hide_text changed: %d -> %d", plugin->hide_text, new_hide_text);
        plugin->hide_text = new_hide_text;
        hide_changed = TRUE;
    }
    if (new_hide_notifications != plugin->hide_notifications) {
        g_debug("  hide_notifications changed: %d -> %d", plugin->hide_notifications, new_hide_notifications);
        plugin->hide_notifications = new_hide_notifications;
        hide_changed = TRUE;
    }
    
    if (hide_changed) {
        update_profile_display(plugin, FALSE);
        g_debug("  Local display settings applied");
    }
    
    /* НАСТРОЙКИ ASUSD */
    gboolean current_ac_enabled = FALSE;
    gboolean current_battery_enabled = FALSE;
    gchar *current_ac_profile = NULL;
    gchar *current_battery_profile = NULL;
    guint8 current_limit = 0;
    
    if (plugin->asusd_state == ASUSD_STATE_AVAILABLE) {
        asusd_get_bool_property(plugin, "ChangePlatformProfileOnAc", &current_ac_enabled);
        asusd_get_bool_property(plugin, "ChangePlatformProfileOnBattery", &current_battery_enabled);
        
        guint32 enum_val;
        if (asusd_get_uint32_property(plugin, "PlatformProfileOnAc", &enum_val)) {
            const gchar *name = profile_name_from_enum(plugin, enum_val);
            current_ac_profile = g_strdup(name);
        }
        if (asusd_get_uint32_property(plugin, "PlatformProfileOnBattery", &enum_val)) {
            const gchar *name = profile_name_from_enum(plugin, enum_val);
            current_battery_profile = g_strdup(name);
        }
        asusd_get_byte_property(plugin, "ChargeControlEndThreshold", &current_limit);
        
        g_debug("  Current ASUSD values:");
        g_debug("    ChangePlatformProfileOnAc = %d", current_ac_enabled);
        g_debug("    ChangePlatformProfileOnBattery = %d", current_battery_enabled);
        g_debug("    PlatformProfileOnAc = %s", current_ac_profile ? current_ac_profile : "NULL");
        g_debug("    PlatformProfileOnBattery = %s", current_battery_profile ? current_battery_profile : "NULL");
        g_debug("    ChargeControlEndThreshold = %d", current_limit);
    } else {
        g_debug("  ASUSD not available, cannot apply settings");
        if (!plugin->hide_notifications) {
            send_notification(_("Error"),
                             _("ASUSD is not available. Cannot apply settings."),
                             TRUE, "emblem-readonly");
        }
        save_settings(plugin);
        return;
    }
    
    /* Получаем значения из диалога */
    gboolean new_ac_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check_ac));
    gboolean new_battery_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check_battery));
    gboolean new_limit_enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(limit_check));
    
    gchar *new_ac_profile = NULL;
    gchar *new_battery_profile = NULL;
    
    GtkTreeIter iter;
    if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(combo_ac), &iter)) {
        GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(combo_ac));
        gtk_tree_model_get(model, &iter, 0, &new_ac_profile, -1);
    }
    
    if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(combo_battery), &iter)) {
        GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(combo_battery));
        gtk_tree_model_get(model, &iter, 0, &new_battery_profile, -1);
    }
    
    g_debug("  Dialog values:");
    g_debug("    ChangePlatformProfileOnAc = %d", new_ac_enabled);
    g_debug("    ChangePlatformProfileOnBattery = %d", new_battery_enabled);
    g_debug("    PlatformProfileOnAc = %s", new_ac_profile ? new_ac_profile : "NULL");
    g_debug("    PlatformProfileOnBattery = %s", new_battery_profile ? new_battery_profile : "NULL");
    g_debug("    ChargeControlEndThreshold (enabled) = %d", new_limit_enabled);
    
    /* Проверяем изменения */
    gboolean has_changes = FALSE;
    
    if (new_ac_enabled != current_ac_enabled) {
        g_debug("  >>> ChangePlatformProfileOnAc changed: %d -> %d", current_ac_enabled, new_ac_enabled);
        has_changes = TRUE;
    }
    
    if (new_battery_enabled != current_battery_enabled) {
        g_debug("  >>> ChangePlatformProfileOnBattery changed: %d -> %d", current_battery_enabled, new_battery_enabled);
        has_changes = TRUE;
    }
    
    if (new_ac_profile && g_strcmp0(new_ac_profile, current_ac_profile) != 0) {
        g_debug("  >>> PlatformProfileOnAc changed: %s -> %s", current_ac_profile, new_ac_profile);
        has_changes = TRUE;
    }
    
    if (new_battery_profile && g_strcmp0(new_battery_profile, current_battery_profile) != 0) {
        g_debug("  >>> PlatformProfileOnBattery changed: %s -> %s", current_battery_profile, new_battery_profile);
        has_changes = TRUE;
    }
    
    guint8 new_limit = new_limit_enabled ? 80 : 100;
    if (new_limit != current_limit) {
        g_debug("  >>> ChargeControlEndThreshold changed: %d -> %d", current_limit, new_limit);
        has_changes = TRUE;
    }
    
    if (!has_changes) {
        g_debug("  No ASUSD changes detected");
        g_free(current_ac_profile);
        g_free(current_battery_profile);
        g_free(new_ac_profile);
        g_free(new_battery_profile);
        save_settings(plugin);
        if (!plugin->hide_notifications) {
            send_notification(_("No changes"),
                             _("Settings are already up to date"),
                             FALSE, "emblem-default");
        }
        return;
    }
    
    g_debug("  Changes detected, applying...");
    
    plugin->saving_settings = TRUE;
    
    if (new_ac_enabled != current_ac_enabled) {
        asusd_set_property(plugin, "ChangePlatformProfileOnAc", 
                          g_variant_new_boolean(new_ac_enabled));
        g_debug("  Applied ChangePlatformProfileOnAc = %d", new_ac_enabled);
    }
    
    if (new_battery_enabled != current_battery_enabled) {
        asusd_set_property(plugin, "ChangePlatformProfileOnBattery", 
                          g_variant_new_boolean(new_battery_enabled));
        g_debug("  Applied ChangePlatformProfileOnBattery = %d", new_battery_enabled);
    }
    
    if (new_ac_profile && g_strcmp0(new_ac_profile, current_ac_profile) != 0) {
        guint32 enum_val = 999;
        if (profile_enum_from_name(plugin, new_ac_profile, &enum_val) && enum_val != 999) {
            asusd_set_property(plugin, "PlatformProfileOnAc", g_variant_new_uint32(enum_val));
            g_debug("  Applied PlatformProfileOnAc = %u", enum_val);
        }
    }
    
    if (new_battery_profile && g_strcmp0(new_battery_profile, current_battery_profile) != 0) {
        guint32 enum_val = 999;
        if (profile_enum_from_name(plugin, new_battery_profile, &enum_val) && enum_val != 999) {
            asusd_set_property(plugin, "PlatformProfileOnBattery", g_variant_new_uint32(enum_val));
            g_debug("  Applied PlatformProfileOnBattery = %u", enum_val);
        }
    }
    
    if (new_limit != current_limit) {
        asusd_set_property(plugin, "ChargeControlEndThreshold", g_variant_new_byte(new_limit));
        g_debug("  Applied ChargeControlEndThreshold = %d", new_limit);
        plugin->battery_limit_enabled = new_limit_enabled;
        plugin->current_battery_limit = new_limit;
    }
    
    plugin->saving_settings = FALSE;
    
    plugin->auto_switch_ac_enabled = new_ac_enabled;
    plugin->auto_switch_battery_enabled = new_battery_enabled;
    if (new_ac_profile) {
        g_free(plugin->auto_switch_ac_profile);
        plugin->auto_switch_ac_profile = g_strdup(new_ac_profile);
    }
    if (new_battery_profile) {
        g_free(plugin->auto_switch_battery_profile);
        plugin->auto_switch_battery_profile = g_strdup(new_battery_profile);
    }
    
    save_settings(plugin);
    g_debug("  Changes applied successfully");
    
    update_profile_display(plugin, FALSE);
    
    if (!plugin->hide_notifications) {
        send_notification(_("Settings applied"),
                         _("Power profile settings have been updated"),
                         FALSE, "emblem-system");
    }
    
    g_free(current_ac_profile);
    g_free(current_battery_profile);
    g_free(new_ac_profile);
    g_free(new_battery_profile);
}

static void on_one_shot_clicked(GtkButton *button, GtkWidget *dialog) {
    GtkWidget *message_dialog;
    gint response;
    
    AsusdBatteryPlugin *plugin = g_object_get_data(G_OBJECT(dialog), "plugin");
    if (!plugin) {
        g_warning("on_one_shot_clicked: plugin is NULL");
        return;
    }

    GtkWidget *limit_check = g_object_get_data(G_OBJECT(dialog), "limit_check");
    gboolean limit_enabled_in_dialog = FALSE;
    if (limit_check) {
        limit_enabled_in_dialog = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(limit_check));
    }

    message_dialog = gtk_message_dialog_new(
        GTK_WINDOW(dialog),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_YES_NO,
        _("Charge battery to 100%% once?\n\n"
          "This will temporarily override the 80%% charge limit.\n"
          "The limit will be restored after the next charge cycle.")
    );

    gtk_window_set_icon_name(GTK_WINDOW(message_dialog), NULL);
    gtk_window_set_title(GTK_WINDOW(message_dialog), _("One-shot full charge"));
    gtk_dialog_set_default_response(GTK_DIALOG(message_dialog), GTK_RESPONSE_NO);

    response = gtk_dialog_run(GTK_DIALOG(message_dialog));
    gtk_widget_destroy(message_dialog);

    if (response == GTK_RESPONSE_YES) {
        if (plugin->dbus_connection) {
            GError *error = NULL;
            
            if (limit_enabled_in_dialog) {
                g_debug("on_one_shot_clicked: applying 80%% limit before one-shot");
                
                GVariant *limit_result = g_dbus_connection_call_sync(
                    plugin->dbus_connection,
                    ASUSD_BUS_NAME,
                    ASUSD_OBJECT_PATH,
                    DBUS_PROPERTIES_INTERFACE,
                    "Set",
                    g_variant_new("(ssv)", ASUSD_INTERFACE, "ChargeControlEndThreshold", g_variant_new_byte(80)),
                    G_VARIANT_TYPE("()"),
                    G_DBUS_CALL_FLAGS_NONE,
                    ASUSD_TIMEOUT_MS,
                    NULL,
                    &error
                );
                
                if (error) {
                    g_warning("Failed to apply battery limit before one-shot: %s", error->message);
                    g_error_free(error);
                    error = NULL;
                } else {
                    if (limit_result) g_variant_unref(limit_result);
                }
            }

            GVariant *result = g_dbus_connection_call_sync(
                plugin->dbus_connection,
                ASUSD_BUS_NAME,
                ASUSD_OBJECT_PATH,
                ASUSD_INTERFACE,
                "OneShotFullCharge",
                NULL,
                NULL,
                G_DBUS_CALL_FLAGS_NONE,
                ASUSD_TIMEOUT_MS,
                NULL,
                &error
            );

            if (error) {
                gchar *error_message = g_strdup(error->message);
                g_warning("Failed to call OneShotFullCharge: %s", error_message);
                g_error_free(error);

                if (!plugin->hide_notifications) {
                    GtkWidget *error_dialog = gtk_message_dialog_new(
                        GTK_WINDOW(dialog),
                        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                        GTK_MESSAGE_ERROR,
                        GTK_BUTTONS_OK,
                        _("Failed to start one-shot full charge:\n%s"),
                        error_message
                    );
                    gtk_window_set_icon_name(GTK_WINDOW(error_dialog), NULL);
                    gtk_dialog_run(GTK_DIALOG(error_dialog));
                    gtk_widget_destroy(error_dialog);
                }
                g_free(error_message);
            } else {
                if (result) g_variant_unref(result);

                if (!plugin->hide_notifications) {
                    GtkWidget *success_dialog = gtk_message_dialog_new(
                        GTK_WINDOW(dialog),
                        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                        GTK_MESSAGE_INFO,
                        GTK_BUTTONS_OK,
                        _("One-shot full charge started.\n\nThe battery will charge to 100%% once.")
                    );
                    gtk_window_set_icon_name(GTK_WINDOW(success_dialog), NULL);
                    gtk_dialog_run(GTK_DIALOG(success_dialog));
                    gtk_widget_destroy(success_dialog);
                }
            }
        }
    }
}

static void on_auto_switch_toggled(GtkToggleButton *toggle_button, GtkWidget *dialog) {
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

static void create_settings_dialog(AsusdBatteryPlugin *plugin) {
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
    int row = 0;

    if (!plugin) return;

    g_debug("=== create_settings_dialog: OPENING ===");
    
    plugin->settings_dialog_open = TRUE;
    plugin->saving_settings = FALSE;

    if (plugin->asusd_state == ASUSD_STATE_AVAILABLE) {
        g_debug("  Reading current settings from ASUSD...");
        
        guint8 limit;
        if (asusd_get_byte_property(plugin, "ChargeControlEndThreshold", &limit)) {
            plugin->current_battery_limit = limit;
            plugin->battery_limit_enabled = (limit == 80);
            g_debug("  battery_limit = %d, enabled = %d", limit, plugin->battery_limit_enabled);
        }
        
        asusd_get_bool_property(plugin, "ChangePlatformProfileOnAc", &plugin->auto_switch_ac_enabled);
        asusd_get_bool_property(plugin, "ChangePlatformProfileOnBattery", &plugin->auto_switch_battery_enabled);
        g_debug("  auto_switch_ac = %d, auto_switch_battery = %d", 
                plugin->auto_switch_ac_enabled, plugin->auto_switch_battery_enabled);
        
        guint32 enum_val;
        if (asusd_get_uint32_property(plugin, "PlatformProfileOnAc", &enum_val)) {
            const gchar *name = profile_name_from_enum(plugin, enum_val);
            g_free(plugin->auto_switch_ac_profile);
            plugin->auto_switch_ac_profile = g_strdup(name);
            g_debug("  auto_switch_ac_profile = %s", plugin->auto_switch_ac_profile);
        }
        
        if (asusd_get_uint32_property(plugin, "PlatformProfileOnBattery", &enum_val)) {
            const gchar *name = profile_name_from_enum(plugin, enum_val);
            g_free(plugin->auto_switch_battery_profile);
            plugin->auto_switch_battery_profile = g_strdup(name);
            g_debug("  auto_switch_battery_profile = %s", plugin->auto_switch_battery_profile);
        }
    } else {
        g_debug("  ASUSD not available, using cached settings");
    }

    /* Создаем диалог БЕЗ стандартной кнопки Close */
    dialog = gtk_dialog_new_with_buttons(_("Power Profile Settings"),
                                        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(plugin->plugin))),
                                        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                        NULL, NULL);

    if (!dialog) {
        g_warning("Failed to create settings dialog");
        plugin->settings_dialog_open = FALSE;
        return;
    }

    g_object_set_data(G_OBJECT(dialog), "plugin", plugin);

    gtk_window_set_icon_name(GTK_WINDOW(dialog), "emblem-system");
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

    content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    if (!content_area) {
        gtk_widget_destroy(dialog);
        plugin->settings_dialog_open = FALSE;
        return;
    }

    gtk_container_set_border_width(GTK_CONTAINER(content_area), 10);

    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    if (!vbox) {
        gtk_widget_destroy(dialog);
        plugin->settings_dialog_open = FALSE;
        return;
    }
    gtk_container_add(GTK_CONTAINER(content_area), vbox);

    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_halign(main_vbox, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), main_vbox, TRUE, TRUE, 0);

    /* ========== Auto switch profiles ========== */
    auto_frame = gtk_frame_new(_("Auto switch profiles"));
    gtk_box_pack_start(GTK_BOX(main_vbox), auto_frame, FALSE, FALSE, 0);

    auto_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(auto_vbox), 5);
    gtk_container_add(GTK_CONTAINER(auto_frame), auto_vbox);

    ac_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_widget_set_halign(ac_hbox, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(auto_vbox), ac_hbox, FALSE, FALSE, 0);

    /* On AC */
    frame_ac = gtk_frame_new(_("On AC"));
    gtk_box_pack_start(GTK_BOX(ac_hbox), frame_ac, FALSE, FALSE, 0);

    hbox_ac = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(hbox_ac), 5);
    gtk_container_add(GTK_CONTAINER(frame_ac), hbox_ac);

    GtkWidget *check_ac = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check_ac), plugin->auto_switch_ac_enabled);
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) {
        gtk_widget_set_sensitive(check_ac, FALSE);
    }
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
            const char *name = settings->name && strlen(settings->name) > 0 ? 
                              settings->name : settings->default_name;
            gtk_list_store_append(store_ac, &iter);
            gtk_list_store_set(store_ac, &iter, 0, name, -1);
            if (plugin->auto_switch_ac_profile && g_strcmp0(name, plugin->auto_switch_ac_profile) == 0) {
                active_index = i;
            }
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo_ac), active_index);
    }
    gtk_widget_set_sensitive(combo_ac, plugin->auto_switch_ac_enabled && 
                                   plugin->asusd_state == ASUSD_STATE_AVAILABLE);
    g_object_set_data(G_OBJECT(dialog), "combo_ac", combo_ac);
    gtk_box_pack_start(GTK_BOX(hbox_ac), combo_ac, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(combo_ac), "changed", G_CALLBACK(on_any_setting_changed), plugin);

    g_signal_connect(G_OBJECT(check_ac), "toggled", G_CALLBACK(on_auto_switch_toggled), dialog);

    /* On Battery */
    frame_battery = gtk_frame_new(_("On Battery"));
    gtk_box_pack_start(GTK_BOX(ac_hbox), frame_battery, FALSE, FALSE, 0);

    hbox_battery = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(hbox_battery), 5);
    gtk_container_add(GTK_CONTAINER(frame_battery), hbox_battery);

    GtkWidget *check_battery = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check_battery), plugin->auto_switch_battery_enabled);
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) {
        gtk_widget_set_sensitive(check_battery, FALSE);
    }
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
            const char *name = settings->name && strlen(settings->name) > 0 ? 
                              settings->name : settings->default_name;
            gtk_list_store_append(store_battery, &iter);
            gtk_list_store_set(store_battery, &iter, 0, name, -1);
            if (plugin->auto_switch_battery_profile && g_strcmp0(name, plugin->auto_switch_battery_profile) == 0) {
                active_index = i;
            }
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo_battery), active_index);
    }
    gtk_widget_set_sensitive(combo_battery, plugin->auto_switch_battery_enabled && 
                                   plugin->asusd_state == ASUSD_STATE_AVAILABLE);
    g_object_set_data(G_OBJECT(dialog), "combo_battery", combo_battery);
    gtk_box_pack_start(GTK_BOX(hbox_battery), combo_battery, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(combo_battery), "changed", G_CALLBACK(on_any_setting_changed), plugin);

    g_signal_connect(G_OBJECT(check_battery), "toggled", G_CALLBACK(on_auto_switch_toggled), dialog);

    /* ========== Battery charge limit ========== */
    GtkWidget *limit_frame = gtk_frame_new(_("Battery charge limit"));
    gtk_box_pack_start(GTK_BOX(main_vbox), limit_frame, FALSE, FALSE, 0);

    GtkWidget *limit_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(limit_vbox), 5);
    gtk_container_add(GTK_CONTAINER(limit_frame), limit_vbox);

    limit_check_widget = gtk_check_button_new_with_label(_("Limit battery charge to 80%"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(limit_check_widget), plugin->battery_limit_enabled);
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) {
        gtk_widget_set_sensitive(limit_check_widget, FALSE);
    }
    g_object_set_data(G_OBJECT(dialog), "limit_check", limit_check_widget);
    gtk_box_pack_start(GTK_BOX(limit_vbox), limit_check_widget, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(limit_check_widget), "toggled", G_CALLBACK(on_any_setting_changed), plugin);

    /* One-shot full charge */
    one_shot_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(limit_vbox), one_shot_hbox, FALSE, FALSE, 0);

    one_shot_label = gtk_label_new(_("Charge battery to 100% once:"));
    gtk_box_pack_start(GTK_BOX(one_shot_hbox), one_shot_label, FALSE, FALSE, 0);

    one_shot_button = gtk_button_new_with_label(_("Start"));
    g_signal_connect(G_OBJECT(one_shot_button), "clicked", G_CALLBACK(on_one_shot_clicked), dialog);
    gtk_box_pack_start(GTK_BOX(one_shot_hbox), one_shot_button, FALSE, FALSE, 0);

    /* ========== Profile names and icons ========== */
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
            if (display_name[0] >= 'a' && display_name[0] <= 'z') {
                display_name[0] = g_ascii_toupper(display_name[0]);
            }
            label = gtk_label_new(display_name);
            gtk_label_set_xalign(GTK_LABEL(label), 0.0);
            gtk_widget_set_size_request(label, 80, -1);
            gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
            g_free(display_name);

            entry_name = gtk_entry_new();
            if (settings->name && strlen(settings->name) > 0) {
                gtk_entry_set_text(GTK_ENTRY(entry_name), settings->name);
            } else {
                gtk_entry_set_placeholder_text(GTK_ENTRY(entry_name), settings->default_name);
            }
            gtk_widget_set_size_request(entry_name, 120, -1);
            g_object_set_data(G_OBJECT(dialog), g_strdup_printf("entry_name_%d", settings->enum_value), entry_name);
            gtk_grid_attach(GTK_GRID(grid), entry_name, 1, row, 1, 1);
            g_signal_connect(G_OBJECT(entry_name), "changed", G_CALLBACK(on_any_setting_changed), plugin);

            entry_icon = gtk_entry_new();
            if (settings->icon && strlen(settings->icon) > 0) {
                gtk_entry_set_text(GTK_ENTRY(entry_icon), settings->icon);
            }
            gtk_entry_set_placeholder_text(GTK_ENTRY(entry_icon), _("icon name"));
            gtk_widget_set_size_request(entry_icon, 120, -1);
            g_object_set_data(G_OBJECT(dialog), g_strdup_printf("entry_icon_%d", settings->enum_value), entry_icon);
            gtk_grid_attach(GTK_GRID(grid), entry_icon, 2, row, 1, 1);
            g_signal_connect(G_OBJECT(entry_icon), "changed", G_CALLBACK(on_any_setting_changed), plugin);

            row++;
        }
    }

    /* ========== Display options ========== */
    GtkWidget *options_frame = gtk_frame_new(_("Display options"));
    gtk_box_pack_start(GTK_BOX(main_vbox), options_frame, FALSE, FALSE, 0);

    GtkWidget *options_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(options_vbox), 5);
    gtk_container_add(GTK_CONTAINER(options_frame), options_vbox);

    GtkWidget *options_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_widget_set_halign(options_hbox, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(options_vbox), options_hbox, FALSE, FALSE, 0);

    GtkWidget *hide_label = gtk_label_new(_("Hide:"));
    gtk_box_pack_start(GTK_BOX(options_hbox), hide_label, FALSE, FALSE, 0);

    hide_icon_check_widget = gtk_check_button_new_with_label(_("Icon"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hide_icon_check_widget), plugin->hide_icon);
    gtk_box_pack_start(GTK_BOX(options_hbox), hide_icon_check_widget, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(dialog), "hide_icon_check", hide_icon_check_widget);
    g_signal_connect(G_OBJECT(hide_icon_check_widget), "toggled", G_CALLBACK(on_hide_toggle), plugin);

    hide_text_check_widget = gtk_check_button_new_with_label(_("Text"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hide_text_check_widget), plugin->hide_text);
    gtk_box_pack_start(GTK_BOX(options_hbox), hide_text_check_widget, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(dialog), "hide_text_check", hide_text_check_widget);
    g_signal_connect(G_OBJECT(hide_text_check_widget), "toggled", G_CALLBACK(on_hide_toggle), plugin);

    notifications_check_widget = gtk_check_button_new_with_label(_("Notifications"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(notifications_check_widget), plugin->hide_notifications);
    gtk_box_pack_start(GTK_BOX(options_hbox), notifications_check_widget, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(dialog), "notifications_check", notifications_check_widget);
    g_signal_connect(G_OBJECT(notifications_check_widget), "toggled", G_CALLBACK(on_any_setting_changed), plugin);

    /* ========== Separator ========== */
    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(main_vbox), separator, FALSE, FALSE, 5);

    /* ========== Button box ========== */
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
    
    g_debug("=== create_settings_dialog: DIALOG SHOWN ===");
}