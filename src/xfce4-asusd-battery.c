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

typedef struct {
    XfcePanelPlugin *plugin;
    GtkWidget *button;
    GtkWidget *box;
    GtkWidget *image;
    GtkWidget *label;
    gchar *current_profile;
    gchar **available_profiles;
    guint timeout_id;
    
    GHashTable *profile_settings;
    GList *profile_order;
    
    gboolean hide_icon;
    gboolean hide_text;
    
    gboolean battery_limit_enabled;
    gboolean auto_switch_ac_enabled;
    gboolean auto_switch_battery_enabled;
    gchar *auto_switch_ac_profile;
    gchar *auto_switch_battery_profile;
    
    GDBusConnection *dbus_connection;
    guint dbus_signal_id;
    guint dbus_name_owner_id;
    gboolean is_on_ac;
    
    gboolean asusd_available;
    GList *supported_profile_list;
    guint asusd_retry_timeout_id;
    guint current_battery_limit;
    gboolean battery_limit_initialized;
} AsusdBatteryPlugin;

typedef struct {
    gchar *name;
    gchar *icon;
    gchar *default_name;
    guint32 enum_value;
} ProfileSettings;

/* Прототипы */
static void asusd_battery_plugin_construct(XfcePanelPlugin *plugin);
static void asusd_battery_plugin_free(gpointer user_data);
static void update_profile_display(AsusdBatteryPlugin *plugin);
static void on_button_clicked(GtkWidget *widget, AsusdBatteryPlugin *plugin);
static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, AsusdBatteryPlugin *plugin);
static void on_profile_selected(GtkMenuItem *item, AsusdBatteryPlugin *plugin);
static void on_menu_configure(GtkMenuItem *item, AsusdBatteryPlugin *plugin);
static void on_menu_about(GtkMenuItem *item, AsusdBatteryPlugin *plugin);
static void send_notification(const gchar *message, const gchar *subtitle, gboolean is_error, const gchar *icon);
static gboolean auto_update(gpointer user_data);
static void load_settings(AsusdBatteryPlugin *plugin);
static void save_settings(AsusdBatteryPlugin *plugin);
static void create_settings_dialog(AsusdBatteryPlugin *plugin);
static void on_settings_response(GtkDialog *dialog, gint response_id, AsusdBatteryPlugin *plugin);
static void on_hide_toggle(GtkToggleButton *toggle_button, GtkWidget *dialog);
static void on_auto_switch_toggled(GtkToggleButton *toggle_button, GtkWidget *dialog);
static void setup_dbus_monitoring(AsusdBatteryPlugin *plugin);
static void on_dbus_signal(GDBusConnection *connection, const gchar *sender_name,
                           const gchar *object_path, const gchar *interface_name,
                           const gchar *signal_name, GVariant *parameters,
                           AsusdBatteryPlugin *plugin);
static void create_about_dialog(AsusdBatteryPlugin *plugin);
static void on_one_shot_clicked(GtkButton *button, GtkWidget *dialog);

/* ASUSD прототипы */
static gboolean asusd_detect_and_init(AsusdBatteryPlugin *plugin);
static void asusd_cleanup(AsusdBatteryPlugin *plugin);
static gboolean asusd_get_current_profile(AsusdBatteryPlugin *plugin, gchar **profile_name);
static gboolean asusd_set_profile(AsusdBatteryPlugin *plugin, const gchar *profile_name);
static gchar** asusd_get_available_profiles(AsusdBatteryPlugin *plugin);
static void asusd_handle_properties_changed(AsusdBatteryPlugin *plugin, GVariant *changed_properties);
static void asusd_handle_name_owner_changed(AsusdBatteryPlugin *plugin, const gchar *old_owner, const gchar *new_owner);
static gboolean asusd_retry_init(gpointer user_data);
static void asusd_update_battery_limit(AsusdBatteryPlugin *plugin);
static void asusd_set_battery_limit(AsusdBatteryPlugin *plugin, guint32 limit);
static void asusd_set_bool_property(AsusdBatteryPlugin *plugin, const char *property, gboolean value);
static void asusd_set_uint32_property(AsusdBatteryPlugin *plugin, const char *property, guint32 value);
static void asusd_sync_all_settings(AsusdBatteryPlugin *plugin);
static guint32 asusd_find_enum_by_name(AsusdBatteryPlugin *plugin, const gchar *profile_name);
static ProfileSettings* profile_settings_new(guint32 enum_value, const gchar *default_name);
static void profile_settings_free(ProfileSettings *settings);

/* Функции для работы с настройками профилей */
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

XFCE_PANEL_PLUGIN_REGISTER(asusd_battery_plugin_construct)

/* ========== ASUSD Функции ========== */

static guint32 asusd_find_enum_by_name(AsusdBatteryPlugin *plugin, const gchar *profile_name) {
    if (!plugin || !profile_name) return 999;
    
    if (plugin->profile_settings) {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, plugin->profile_settings);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            ProfileSettings *settings = (ProfileSettings*)value;
            if (settings->name && g_strcmp0(profile_name, settings->name) == 0) {
                return settings->enum_value;
            }
        }
    }
    
    GList *iter;
    for (iter = plugin->supported_profile_list; iter != NULL; iter = iter->next) {
        guint32 enum_val = GPOINTER_TO_UINT(iter->data);
        const char *default_name = asusd_enum_to_default_name(enum_val);
        if (g_strcmp0(profile_name, default_name) == 0) {
            return enum_val;
        }
    }
    
    if (plugin->profile_settings) {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, plugin->profile_settings);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            ProfileSettings *settings = (ProfileSettings*)value;
            if (settings->default_name && g_strcmp0(profile_name, settings->default_name) == 0) {
                return settings->enum_value;
            }
        }
    }
    
    return 999;
}

static gboolean asusd_check_service(GDBusConnection *connection) {
    if (!connection) return FALSE;
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        connection,
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
    if (error) {
        g_error_free(error);
        return FALSE;
    }
    g_variant_unref(result);
    return TRUE;
}

static GVariant* asusd_get_property_simple(GDBusConnection *connection, 
                                           const char *property_name,
                                           GError **error) {
    if (!connection || !property_name) return NULL;
    
    GVariant *result = g_dbus_connection_call_sync(
        connection,
        ASUSD_BUS_NAME,
        ASUSD_OBJECT_PATH,
        DBUS_PROPERTIES_INTERFACE,
        "Get",
        g_variant_new("(ss)", ASUSD_INTERFACE, property_name),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        ASUSD_TIMEOUT_MS,
        NULL,
        error
    );
    
    if (error && *error) return NULL;
    if (!result) return NULL;
    
    GVariant *value = NULL;
    g_variant_get(result, "(v)", &value);
    g_variant_unref(result);
    
    return value;
}

static void asusd_set_bool_property(AsusdBatteryPlugin *plugin, const char *property, gboolean value) {
    if (!plugin || !plugin->dbus_connection || !property) return;
    
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        plugin->dbus_connection,
        ASUSD_BUS_NAME,
        ASUSD_OBJECT_PATH,
        DBUS_PROPERTIES_INTERFACE,
        "Set",
        g_variant_new("(ssv)", ASUSD_INTERFACE, property, g_variant_new_boolean(value)),
        G_VARIANT_TYPE("()"),
        G_DBUS_CALL_FLAGS_NONE,
        ASUSD_TIMEOUT_MS,
        NULL,
        &error
    );
    
    if (error) {
        g_warning("ASUSD: Failed to set %s: %s", property, error->message);
        g_error_free(error);
    } else {
        if (result) g_variant_unref(result);
    }
}

static void asusd_set_uint32_property(AsusdBatteryPlugin *plugin, const char *property, guint32 value) {
    if (!plugin || !plugin->dbus_connection || !property) return;
    
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        plugin->dbus_connection,
        ASUSD_BUS_NAME,
        ASUSD_OBJECT_PATH,
        DBUS_PROPERTIES_INTERFACE,
        "Set",
        g_variant_new("(ssv)", ASUSD_INTERFACE, property, g_variant_new_uint32(value)),
        G_VARIANT_TYPE("()"),
        G_DBUS_CALL_FLAGS_NONE,
        ASUSD_TIMEOUT_MS,
        NULL,
        &error
    );
    
    if (error) {
        g_warning("ASUSD: Failed to set %s: %s", property, error->message);
        g_error_free(error);
    } else {
        if (result) g_variant_unref(result);
    }
}

static void asusd_set_battery_limit(AsusdBatteryPlugin *plugin, guint32 limit) {
    if (!plugin || !plugin->dbus_connection) return;
    
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        plugin->dbus_connection,
        ASUSD_BUS_NAME,
        ASUSD_OBJECT_PATH,
        DBUS_PROPERTIES_INTERFACE,
        "Set",
        g_variant_new("(ssv)", ASUSD_INTERFACE, "ChargeControlEndThreshold", g_variant_new_byte((guint8)limit)),
        G_VARIANT_TYPE("()"),
        G_DBUS_CALL_FLAGS_NONE,
        ASUSD_TIMEOUT_MS,
        NULL,
        &error
    );
    
    if (error) {
        g_warning("ASUSD: Failed to set battery limit: %s", error->message);
        g_error_free(error);
    } else {
        if (result) g_variant_unref(result);
        plugin->current_battery_limit = limit;
    }
}

static void asusd_sync_all_settings(AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->asusd_available || !plugin->dbus_connection) return;
    
    GError *error = NULL;
    
    asusd_update_battery_limit(plugin);
    
    GVariant *val_ac = asusd_get_property_simple(plugin->dbus_connection, "ChangePlatformProfileOnAc", &error);
    if (!error && val_ac) {
        if (g_variant_is_of_type(val_ac, G_VARIANT_TYPE_VARIANT)) {
            GVariant *unwrapped = g_variant_get_variant(val_ac);
            g_variant_unref(val_ac);
            val_ac = unwrapped;
        }
        if (g_variant_is_of_type(val_ac, G_VARIANT_TYPE_BOOLEAN)) {
            gboolean enabled;
            g_variant_get(val_ac, "b", &enabled);
            plugin->auto_switch_ac_enabled = enabled;
        }
        g_variant_unref(val_ac);
    } else if (error) {
        g_error_free(error);
        error = NULL;
    }
    
    GVariant *val_bat = asusd_get_property_simple(plugin->dbus_connection, "ChangePlatformProfileOnBattery", &error);
    if (!error && val_bat) {
        if (g_variant_is_of_type(val_bat, G_VARIANT_TYPE_VARIANT)) {
            GVariant *unwrapped = g_variant_get_variant(val_bat);
            g_variant_unref(val_bat);
            val_bat = unwrapped;
        }
        if (g_variant_is_of_type(val_bat, G_VARIANT_TYPE_BOOLEAN)) {
            gboolean enabled;
            g_variant_get(val_bat, "b", &enabled);
            plugin->auto_switch_battery_enabled = enabled;
        }
        g_variant_unref(val_bat);
    } else if (error) {
        g_error_free(error);
        error = NULL;
    }
    
    GVariant *val_ac_profile = asusd_get_property_simple(plugin->dbus_connection, "PlatformProfileOnAc", &error);
    if (!error && val_ac_profile) {
        if (g_variant_is_of_type(val_ac_profile, G_VARIANT_TYPE_VARIANT)) {
            GVariant *unwrapped = g_variant_get_variant(val_ac_profile);
            g_variant_unref(val_ac_profile);
            val_ac_profile = unwrapped;
        }
        if (g_variant_is_of_type(val_ac_profile, G_VARIANT_TYPE_UINT32)) {
            guint32 enum_val;
            g_variant_get(val_ac_profile, "u", &enum_val);
            const char *name = asusd_enum_to_default_name(enum_val);
            
            ProfileSettings *settings = g_hash_table_lookup(plugin->profile_settings, g_strdup_printf("%d", enum_val));
            if (settings && settings->name && strlen(settings->name) > 0) {
                g_free(plugin->auto_switch_ac_profile);
                plugin->auto_switch_ac_profile = g_strdup(settings->name);
            } else {
                g_free(plugin->auto_switch_ac_profile);
                plugin->auto_switch_ac_profile = g_strdup(name);
            }
        }
        g_variant_unref(val_ac_profile);
    } else if (error) {
        g_error_free(error);
        error = NULL;
    }
    
    GVariant *val_bat_profile = asusd_get_property_simple(plugin->dbus_connection, "PlatformProfileOnBattery", &error);
    if (!error && val_bat_profile) {
        if (g_variant_is_of_type(val_bat_profile, G_VARIANT_TYPE_VARIANT)) {
            GVariant *unwrapped = g_variant_get_variant(val_bat_profile);
            g_variant_unref(val_bat_profile);
            val_bat_profile = unwrapped;
        }
        if (g_variant_is_of_type(val_bat_profile, G_VARIANT_TYPE_UINT32)) {
            guint32 enum_val;
            g_variant_get(val_bat_profile, "u", &enum_val);
            const char *name = asusd_enum_to_default_name(enum_val);
            
            ProfileSettings *settings = g_hash_table_lookup(plugin->profile_settings, g_strdup_printf("%d", enum_val));
            if (settings && settings->name && strlen(settings->name) > 0) {
                g_free(plugin->auto_switch_battery_profile);
                plugin->auto_switch_battery_profile = g_strdup(settings->name);
            } else {
                g_free(plugin->auto_switch_battery_profile);
                plugin->auto_switch_battery_profile = g_strdup(name);
            }
        }
        g_variant_unref(val_bat_profile);
    } else if (error) {
        g_error_free(error);
        error = NULL;
    }
}

static void asusd_update_battery_limit(AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->dbus_connection) return;
    
    GError *error = NULL;
    GVariant *value = asusd_get_property_simple(plugin->dbus_connection, 
                                                "ChargeControlEndThreshold",
                                                &error);
    if (error) {
        g_error_free(error);
        return;
    }
    
    if (!value) return;
    
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
        GVariant *unwrapped = g_variant_get_variant(value);
        g_variant_unref(value);
        value = unwrapped;
    }
    
    guint8 limit = 0;
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_BYTE)) {
        g_variant_get(value, "y", &limit);
    } else if (g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32)) {
        guint32 limit32;
        g_variant_get(value, "u", &limit32);
        limit = (guint8)limit32;
    } else {
        g_variant_unref(value);
        return;
    }
    g_variant_unref(value);
    
    plugin->current_battery_limit = limit;
    plugin->battery_limit_initialized = TRUE;
    
    gboolean should_be_enabled = (limit == 80);
    if (plugin->battery_limit_enabled != should_be_enabled) {
        plugin->battery_limit_enabled = should_be_enabled;
    }
}

static gboolean asusd_load_supported_profiles(AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->dbus_connection) return FALSE;
    
    GError *error = NULL;
    GVariant *value = asusd_get_property_simple(plugin->dbus_connection, 
                                                "PlatformProfileChoices",
                                                &error);
    if (error) {
        g_warning("ASUSD: Failed to get PlatformProfileChoices: %s", error->message);
        g_error_free(error);
        return FALSE;
    }
    
    if (!value) {
        g_warning("ASUSD: No value for PlatformProfileChoices");
        return FALSE;
    }
    
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
        GVariant *unwrapped = g_variant_get_variant(value);
        g_variant_unref(value);
        value = unwrapped;
    }
    
    if (!g_variant_is_of_type(value, G_VARIANT_TYPE_ARRAY)) {
        g_warning("ASUSD: PlatformProfileChoices is not an array");
        g_variant_unref(value);
        return FALSE;
    }
    
    if (plugin->supported_profile_list) {
        g_list_free(plugin->supported_profile_list);
        plugin->supported_profile_list = NULL;
    }
    
    if (plugin->profile_settings) {
        g_hash_table_destroy(plugin->profile_settings);
        plugin->profile_settings = NULL;
    }
    if (plugin->profile_order) {
        g_list_free(plugin->profile_order);
        plugin->profile_order = NULL;
    }
    
    plugin->profile_settings = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)profile_settings_free);
    
    GVariantIter iter;
    guint32 enum_value;
    g_variant_iter_init(&iter, value);
    
    XfconfChannel *channel = xfconf_channel_get(CONFIG_CHANNEL);
    
    while (g_variant_iter_next(&iter, "u", &enum_value)) {
        plugin->supported_profile_list = g_list_append(
            plugin->supported_profile_list, 
            GUINT_TO_POINTER(enum_value)
        );
        
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
        
        g_hash_table_insert(plugin->profile_settings, 
                           g_strdup_printf("%d", enum_value), 
                           settings);
        plugin->profile_order = g_list_append(plugin->profile_order, GUINT_TO_POINTER(enum_value));
    }
    
    g_variant_unref(value);
    return TRUE;
}

static gboolean asusd_get_current_profile(AsusdBatteryPlugin *plugin, gchar **profile_name) {
    if (!plugin || !plugin->dbus_connection || !profile_name) return FALSE;
    
    GError *error = NULL;
    GVariant *value = asusd_get_property_simple(plugin->dbus_connection, 
                                                "PlatformProfile",
                                                &error);
    if (error) {
        g_warning("ASUSD: Failed to get current profile: %s", error->message);
        g_error_free(error);
        *profile_name = g_strdup("unknown");
        return FALSE;
    }
    
    if (!value) {
        *profile_name = g_strdup("unknown");
        return FALSE;
    }
    
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
        GVariant *unwrapped = g_variant_get_variant(value);
        g_variant_unref(value);
        value = unwrapped;
    }
    
    guint32 enum_value;
    if (!g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32)) {
        g_warning("ASUSD: PlatformProfile is not uint32");
        g_variant_unref(value);
        *profile_name = g_strdup("unknown");
        return FALSE;
    }
    
    g_variant_get(value, "u", &enum_value);
    g_variant_unref(value);
    
    ProfileSettings *settings = g_hash_table_lookup(plugin->profile_settings, 
                                                    g_strdup_printf("%d", enum_value));
    if (settings && settings->name && strlen(settings->name) > 0) {
        *profile_name = g_strdup(settings->name);
    } else if (settings) {
        *profile_name = g_strdup(settings->default_name);
    } else {
        *profile_name = g_strdup(asusd_enum_to_default_name(enum_value));
    }
    return TRUE;
}

static gboolean asusd_set_profile(AsusdBatteryPlugin *plugin, const gchar *profile_name) {
    if (!plugin || !plugin->dbus_connection || !profile_name) return FALSE;
    
    guint32 enum_val = asusd_find_enum_by_name(plugin, profile_name);
    
    if (enum_val == 999) {
        g_warning("ASUSD: Profile %s not found", profile_name);
        return FALSE;
    }
    
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_sync(
        plugin->dbus_connection,
        ASUSD_BUS_NAME,
        ASUSD_OBJECT_PATH,
        DBUS_PROPERTIES_INTERFACE,
        "Set",
        g_variant_new("(ssv)", ASUSD_INTERFACE, "PlatformProfile", g_variant_new_uint32(enum_val)),
        G_VARIANT_TYPE("()"),
        G_DBUS_CALL_FLAGS_NONE,
        ASUSD_TIMEOUT_MS,
        NULL,
        &error
    );
    
    if (error) {
        g_warning("ASUSD: Failed to set profile: %s", error->message);
        g_error_free(error);
        return FALSE;
    }
    
    if (result) g_variant_unref(result);
    return TRUE;
}

static gchar** asusd_get_available_profiles(AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->supported_profile_list) return NULL;
    
    GPtrArray *profiles = g_ptr_array_new();
    GList *iter;
    
    for (iter = plugin->profile_order; iter != NULL; iter = iter->next) {
        guint32 enum_val = GPOINTER_TO_UINT(iter->data);
        ProfileSettings *settings = g_hash_table_lookup(plugin->profile_settings, 
                                                        g_strdup_printf("%d", enum_val));
        
        const char *display_name = NULL;
        if (settings && settings->name && strlen(settings->name) > 0) {
            display_name = settings->name;
        } else if (settings) {
            display_name = settings->default_name;
        } else {
            display_name = asusd_enum_to_default_name(enum_val);
        }
        
        if (display_name) {
            g_ptr_array_add(profiles, g_strdup(display_name));
        }
    }
    
    g_ptr_array_add(profiles, NULL);
    return (gchar**)g_ptr_array_free(profiles, FALSE);
}

static void asusd_handle_properties_changed(AsusdBatteryPlugin *plugin, GVariant *changed_properties) {
    if (!plugin || !changed_properties) return;
    
    GVariant *profile_variant = g_variant_lookup_value(changed_properties, 
                                                      "PlatformProfile", 
                                                      G_VARIANT_TYPE_UINT32);
    if (profile_variant) {
        guint32 enum_value;
        g_variant_get(profile_variant, "u", &enum_value);
        g_variant_unref(profile_variant);
        
        ProfileSettings *settings = g_hash_table_lookup(plugin->profile_settings, 
                                                        g_strdup_printf("%d", enum_value));
        const char *name = NULL;
        if (settings && settings->name && strlen(settings->name) > 0) {
            name = settings->name;
        } else if (settings) {
            name = settings->default_name;
        } else {
            name = asusd_enum_to_default_name(enum_value);
        }
        
        g_free(plugin->current_profile);
        plugin->current_profile = g_strdup(name);
        update_profile_display(plugin);
    }
    
    GVariant *limit_variant = g_variant_lookup_value(changed_properties, 
                                                    "ChargeControlEndThreshold", 
                                                    G_VARIANT_TYPE_BYTE);
    if (limit_variant) {
        guint8 limit;
        g_variant_get(limit_variant, "y", &limit);
        g_variant_unref(limit_variant);
        
        plugin->current_battery_limit = limit;
        plugin->battery_limit_initialized = TRUE;
        
        gboolean should_be_enabled = (limit == 80);
        if (plugin->battery_limit_enabled != should_be_enabled) {
            plugin->battery_limit_enabled = should_be_enabled;
        }
    } else {
        GVariant *limit_variant_u32 = g_variant_lookup_value(changed_properties, 
                                                            "ChargeControlEndThreshold", 
                                                            G_VARIANT_TYPE_UINT32);
        if (limit_variant_u32) {
            guint32 limit;
            g_variant_get(limit_variant_u32, "u", &limit);
            g_variant_unref(limit_variant_u32);
            
            plugin->current_battery_limit = limit;
            plugin->battery_limit_initialized = TRUE;
            
            gboolean should_be_enabled = (limit == 80);
            if (plugin->battery_limit_enabled != should_be_enabled) {
                plugin->battery_limit_enabled = should_be_enabled;
            }
        }
    }
    
    if (g_variant_lookup_value(changed_properties, "ChangePlatformProfileOnAc", G_VARIANT_TYPE_BOOLEAN) ||
        g_variant_lookup_value(changed_properties, "ChangePlatformProfileOnBattery", G_VARIANT_TYPE_BOOLEAN) ||
        g_variant_lookup_value(changed_properties, "PlatformProfileOnAc", G_VARIANT_TYPE_UINT32) ||
        g_variant_lookup_value(changed_properties, "PlatformProfileOnBattery", G_VARIANT_TYPE_UINT32)) {
        asusd_sync_all_settings(plugin);
    }
}

static void asusd_handle_name_owner_changed(AsusdBatteryPlugin *plugin, 
                                            const gchar *old_owner, 
                                            const gchar *new_owner) {
    if (!plugin) return;
    
    if (new_owner && strlen(new_owner) > 0) {
        if (plugin->asusd_retry_timeout_id > 0) {
            g_source_remove(plugin->asusd_retry_timeout_id);
            plugin->asusd_retry_timeout_id = 0;
        }
        asusd_detect_and_init(plugin);
    } else {
        plugin->asusd_available = FALSE;
        if (plugin->asusd_retry_timeout_id == 0) {
            plugin->asusd_retry_timeout_id = g_timeout_add_seconds(5, asusd_retry_init, plugin);
        }
    }
}

static gboolean asusd_retry_init(gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin *)user_data;
    if (!plugin) return FALSE;
    plugin->asusd_retry_timeout_id = 0;
    if (!plugin->asusd_available) {
        asusd_detect_and_init(plugin);
    }
    return FALSE;
}

static gboolean asusd_detect_and_init(AsusdBatteryPlugin *plugin) {
    if (!plugin) return FALSE;
    
    if (!plugin->dbus_connection) {
        GError *error = NULL;
        plugin->dbus_connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
        if (error) {
            g_warning("ASUSD: Failed to connect to system bus: %s", error->message);
            g_error_free(error);
            return FALSE;
        }
    }
    
    if (!asusd_check_service(plugin->dbus_connection)) {
        plugin->asusd_available = FALSE;
        return FALSE;
    }
    
    if (!asusd_load_supported_profiles(plugin)) {
        plugin->asusd_available = FALSE;
        return FALSE;
    }
    
    gchar *profile_name = NULL;
    if (!asusd_get_current_profile(plugin, &profile_name)) {
        plugin->asusd_available = FALSE;
        return FALSE;
    }
    
    if (profile_name) {
        g_free(plugin->current_profile);
        plugin->current_profile = profile_name;
    }
    
    asusd_sync_all_settings(plugin);
    
    if (plugin->dbus_signal_id == 0) {
        plugin->dbus_signal_id = g_dbus_connection_signal_subscribe(
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
    }
    
    if (plugin->dbus_name_owner_id == 0) {
        plugin->dbus_name_owner_id = g_dbus_connection_signal_subscribe(
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
    }
    
    plugin->asusd_available = TRUE;
    update_profile_display(plugin);
    
    return TRUE;
}

static void asusd_cleanup(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    
    if (plugin->dbus_signal_id > 0 && plugin->dbus_connection) {
        g_dbus_connection_signal_unsubscribe(plugin->dbus_connection, plugin->dbus_signal_id);
        plugin->dbus_signal_id = 0;
    }
    
    if (plugin->dbus_name_owner_id > 0 && plugin->dbus_connection) {
        g_dbus_connection_signal_unsubscribe(plugin->dbus_connection, plugin->dbus_name_owner_id);
        plugin->dbus_name_owner_id = 0;
    }
    
    if (plugin->supported_profile_list) {
        g_list_free(plugin->supported_profile_list);
        plugin->supported_profile_list = NULL;
    }
    
    if (plugin->profile_settings) {
        g_hash_table_destroy(plugin->profile_settings);
        plugin->profile_settings = NULL;
    }
    
    if (plugin->profile_order) {
        g_list_free(plugin->profile_order);
        plugin->profile_order = NULL;
    }
    
    if (plugin->asusd_retry_timeout_id > 0) {
        g_source_remove(plugin->asusd_retry_timeout_id);
        plugin->asusd_retry_timeout_id = 0;
    }
    
    if (plugin->dbus_connection) {
        g_object_unref(plugin->dbus_connection);
        plugin->dbus_connection = NULL;
    }
    
    plugin->asusd_available = FALSE;
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
    plugin_data->available_profiles = NULL;
    plugin_data->hide_icon = FALSE;
    plugin_data->hide_text = FALSE;
    plugin_data->auto_switch_ac_enabled = FALSE;
    plugin_data->auto_switch_battery_enabled = FALSE;
    plugin_data->auto_switch_ac_profile = NULL;
    plugin_data->auto_switch_battery_profile = NULL;
    plugin_data->is_on_ac = TRUE;
    plugin_data->dbus_connection = NULL;
    plugin_data->dbus_signal_id = 0;
    plugin_data->dbus_name_owner_id = 0;
    plugin_data->asusd_available = FALSE;
    plugin_data->supported_profile_list = NULL;
    plugin_data->profile_settings = NULL;
    plugin_data->profile_order = NULL;
    plugin_data->asusd_retry_timeout_id = 0;
    plugin_data->current_battery_limit = 0;
    plugin_data->battery_limit_initialized = FALSE;
    plugin_data->battery_limit_enabled = FALSE;

    load_settings(plugin_data);

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

    asusd_detect_and_init(plugin_data);
    
    if (!plugin_data->asusd_available) {
        send_notification(_("ASUSD not available"), 
                         _("Please install asusd to manage power profiles"), 
                         TRUE, "battery-missing-symbolic");
    }

    update_profile_display(plugin_data);

    g_signal_connect(G_OBJECT(plugin_data->button), "clicked", G_CALLBACK(on_button_clicked), plugin_data);
    g_signal_connect(G_OBJECT(plugin_data->button), "button-press-event", G_CALLBACK(on_button_press), plugin_data);

    gtk_container_add(GTK_CONTAINER(plugin), plugin_data->button);
    gtk_widget_show_all(plugin_data->button);

    plugin_data->timeout_id = g_timeout_add_seconds(5, auto_update, plugin_data);
    
    setup_dbus_monitoring(plugin_data);

    g_object_set_data_full(G_OBJECT(plugin), "plugin_data", plugin_data, asusd_battery_plugin_free);
}

static void asusd_battery_plugin_free(gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin *)user_data;
    if (!plugin) return;
    
    if (plugin->timeout_id > 0) {
        g_source_remove(plugin->timeout_id);
        plugin->timeout_id = 0;
    }
    
    asusd_cleanup(plugin);
    
    g_free(plugin->current_profile);
    g_free(plugin->auto_switch_ac_profile);
    g_free(plugin->auto_switch_battery_profile);
    
    if (plugin->available_profiles) {
        g_strfreev(plugin->available_profiles);
        plugin->available_profiles = NULL;
    }
    
    g_free(plugin);
}

static void send_notification(const gchar *message, const gchar *subtitle, gboolean is_error, const gchar *icon) {
    gchar *command;
    const gchar *icon_name = icon ? icon : (is_error ? "dialog-error" : "battery-good-symbolic");
    const gchar *urgency = is_error ? "critical" : "normal";

    if (!message) return;

    command = g_strdup_printf(
        "notify-send --urgency=%s --icon=%s \"%s\" \"%s\"",
        urgency, icon_name, message, subtitle ? subtitle : ""
    );
    if (command) {
        g_spawn_command_line_async(command, NULL);
        g_free(command);
    }
}

static void load_settings(AsusdBatteryPlugin *plugin) {
    XfconfChannel *channel;
    
    if (!plugin) return;
    
    channel = xfconf_channel_get(CONFIG_CHANNEL);
    if (!channel) {
        g_warning("Failed to get xfconf channel");
        return;
    }
    
    plugin->hide_icon = xfconf_channel_get_bool(channel, 
        CONFIG_PROPERTY_PREFIX "/hide_icon", FALSE);
    
    plugin->hide_text = xfconf_channel_get_bool(channel, 
        CONFIG_PROPERTY_PREFIX "/hide_text", FALSE);
}

static void save_settings(AsusdBatteryPlugin *plugin) {
    XfconfChannel *channel;
    gchar *property;
    
    if (!plugin) return;
    
    channel = xfconf_channel_get(CONFIG_CHANNEL);
    if (!channel) {
        g_warning("Failed to get xfconf channel");
        return;
    }
    
    xfconf_channel_set_bool(channel, CONFIG_PROPERTY_PREFIX "/hide_icon", plugin->hide_icon);
    xfconf_channel_set_bool(channel, CONFIG_PROPERTY_PREFIX "/hide_text", plugin->hide_text);
    
    if (plugin->profile_settings) {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, plugin->profile_settings);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            ProfileSettings *settings = (ProfileSettings*)value;
            property = g_strdup_printf("%s/profile_%d_name", CONFIG_PROPERTY_PREFIX, settings->enum_value);
            if (settings->name && strlen(settings->name) > 0) {
                xfconf_channel_set_string(channel, property, settings->name);
            } else {
                xfconf_channel_set_string(channel, property, "");
            }
            g_free(property);
            
            property = g_strdup_printf("%s/profile_%d_icon", CONFIG_PROPERTY_PREFIX, settings->enum_value);
            if (settings->icon && strlen(settings->icon) > 0) {
                xfconf_channel_set_string(channel, property, settings->icon);
            } else {
                xfconf_channel_set_string(channel, property, "");
            }
            g_free(property);
        }
    }
}

static void setup_dbus_monitoring(AsusdBatteryPlugin *plugin) {
    GError *error = NULL;
    
    if (!plugin) return;
    
    if (!plugin->dbus_connection) {
        plugin->dbus_connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
        if (!plugin->dbus_connection) {
            g_warning("Failed to connect to system bus: %s", error ? error->message : "unknown");
            if (error) g_error_free(error);
            return;
        }
    }
    
    plugin->dbus_signal_id = g_dbus_connection_signal_subscribe(
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
    
    if (plugin->dbus_signal_id == 0) {
        g_warning("Failed to subscribe to UPower D-Bus signal");
    }
}

static void on_dbus_signal(GDBusConnection *connection, const gchar *sender_name,
                           const gchar *object_path, const gchar *interface_name,
                           const gchar *signal_name, GVariant *parameters,
                           AsusdBatteryPlugin *plugin) {
    if (!plugin || !parameters) return;
    
    if (g_strcmp0(sender_name, ASUSD_BUS_NAME) == 0 &&
        g_strcmp0(signal_name, "PropertiesChanged") == 0 &&
        g_strcmp0(object_path, ASUSD_OBJECT_PATH) == 0) {
        
        const char *changed_interface = NULL;
        GVariant *changed_properties = NULL;
        GVariant *invalidated_properties = NULL;
        
        g_variant_get(parameters, "(&sa{sv}as)", 
                      &changed_interface, &changed_properties, &invalidated_properties);
        
        if (g_strcmp0(changed_interface, ASUSD_INTERFACE) == 0) {
            asusd_handle_properties_changed(plugin, changed_properties);
        }
        
        if (changed_properties) g_variant_unref(changed_properties);
        if (invalidated_properties) g_variant_unref(invalidated_properties);
        return;
    }
    
    if (g_strcmp0(sender_name, "org.freedesktop.DBus") == 0 &&
        g_strcmp0(signal_name, DBUS_NAME_OWNER_CHANGED) == 0 &&
        g_strcmp0(object_path, "/org/freedesktop/DBus") == 0) {
        
        const char *name, *old_owner, *new_owner;
        g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
        
        if (g_strcmp0(name, ASUSD_BUS_NAME) == 0) {
            asusd_handle_name_owner_changed(plugin, old_owner, new_owner);
        }
        return;
    }
    
    if (g_strcmp0(signal_name, "PropertiesChanged") != 0) return;
    
    const gchar *interface = NULL;
    GVariant *changed_properties = NULL;
    GVariant *invalidated_properties = NULL;
    gboolean on_battery = FALSE;
    gboolean status_changed = FALSE;
    
    g_variant_get(parameters, "(&s@a{sv}@as)", 
                  &interface, &changed_properties, &invalidated_properties);
    
    if (g_strcmp0(interface, "org.freedesktop.UPower") != 0) {
        if (changed_properties) g_variant_unref(changed_properties);
        if (invalidated_properties) g_variant_unref(invalidated_properties);
        return;
    }
    
    if (changed_properties) {
        GVariantDict dict;
        GVariant *value = NULL;
        
        g_variant_dict_init(&dict, changed_properties);
        value = g_variant_dict_lookup_value(&dict, "OnBattery", NULL);
        
        if (value) {
            on_battery = g_variant_get_boolean(value);
            status_changed = TRUE;
            g_variant_unref(value);
        }
        
        g_variant_dict_clear(&dict);
        g_variant_unref(changed_properties);
    }
    
    if (invalidated_properties) {
        g_variant_unref(invalidated_properties);
    }
    
    if (status_changed) {
        plugin->is_on_ac = !on_battery;
    }
}

static void update_profile_display(AsusdBatteryPlugin *plugin) {
    gchar *profile = NULL;
    gchar *display_text;

    if (!plugin) return;

    if (plugin->asusd_available) {
        gchar *asusd_profile = NULL;
        if (asusd_get_current_profile(plugin, &asusd_profile)) {
            g_free(plugin->current_profile);
            plugin->current_profile = asusd_profile;
            profile = asusd_profile;
        } else {
            profile = plugin->current_profile;
        }
    } else {
        profile = plugin->current_profile;
    }

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
        if (plugin->available_profiles) {
            g_strfreev(plugin->available_profiles);
            plugin->available_profiles = NULL;
        }
        
        if (plugin->asusd_available) {
            plugin->available_profiles = asusd_get_available_profiles(plugin);
        }
        
        gchar *icon_name = NULL;
        if (plugin->profile_settings) {
            GHashTableIter iter;
            gpointer key, value;
            g_hash_table_iter_init(&iter, plugin->profile_settings);
            while (g_hash_table_iter_next(&iter, &key, &value)) {
                ProfileSettings *settings = (ProfileSettings*)value;
                if (settings->name && g_strcmp0(profile, settings->name) == 0) {
                    if (settings->icon && strlen(settings->icon) > 0) {
                        icon_name = g_strdup(settings->icon);
                    }
                    break;
                }
                if (settings->default_name && g_strcmp0(profile, settings->default_name) == 0) {
                    if (settings->icon && strlen(settings->icon) > 0) {
                        icon_name = g_strdup(settings->icon);
                    }
                    break;
                }
            }
        }
        
        if (plugin->hide_text) {
            gtk_label_set_text(GTK_LABEL(plugin->label), "");
        } else {
            display_text = g_strdup(profile);
            if (g_strcmp0(profile, "balanced") == 0 ||
                g_strcmp0(profile, "performance") == 0 ||
                g_strcmp0(profile, "quiet") == 0) {
                if (strlen(display_text) > 0) {
                    display_text[0] = g_ascii_toupper(display_text[0]);
                }
            }
            gtk_label_set_text(GTK_LABEL(plugin->label), display_text);
            g_free(display_text);
        }

        if (icon_name) {
            gtk_image_set_from_icon_name(GTK_IMAGE(plugin->image), icon_name, GTK_ICON_SIZE_MENU);
            g_free(icon_name);
        } else {
            if (g_strcmp0(profile, "performance") == 0) {
                gtk_image_set_from_icon_name(GTK_IMAGE(plugin->image), "battery-full-symbolic", GTK_ICON_SIZE_MENU);
            } else if (g_strcmp0(profile, "balanced") == 0) {
                gtk_image_set_from_icon_name(GTK_IMAGE(plugin->image), "battery-good-symbolic", GTK_ICON_SIZE_MENU);
            } else if (g_strcmp0(profile, "quiet") == 0 || g_strcmp0(profile, "low-power") == 0) {
                gtk_image_set_from_icon_name(GTK_IMAGE(plugin->image), "battery-low-symbolic", GTK_ICON_SIZE_MENU);
            } else {
                gtk_image_set_from_icon_name(GTK_IMAGE(plugin->image), "battery-good-symbolic", GTK_ICON_SIZE_MENU);
            }
        }
    } else {
        gtk_label_set_text(GTK_LABEL(plugin->label), "?");
        gtk_image_set_from_icon_name(GTK_IMAGE(plugin->image), "battery-missing-symbolic", GTK_ICON_SIZE_MENU);
    }
}

static gboolean auto_update(gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin *)user_data;
    if (!plugin) return FALSE;
    update_profile_display(plugin);
    return TRUE;
}

static void on_button_clicked(GtkWidget *widget, AsusdBatteryPlugin *plugin) {
    GtkWidget *menu;
    GtkWidget *item;
    GtkWidget *hbox;
    GtkWidget *image;
    GtkWidget *label_widget;
    gchar **profiles;
    gchar *current;

    if (!plugin || !widget || !plugin->asusd_available) return;

    menu = gtk_menu_new();
    if (!menu) return;

    if (plugin->available_profiles) {
        g_strfreev(plugin->available_profiles);
        plugin->available_profiles = NULL;
    }
    
    profiles = asusd_get_available_profiles(plugin);
    plugin->available_profiles = profiles;
    
    gchar *asusd_current = NULL;
    if (asusd_get_current_profile(plugin, &asusd_current)) {
        g_free(plugin->current_profile);
        plugin->current_profile = asusd_current;
    }
    current = plugin->current_profile;

    if (profiles) {
        for (int i = 0; profiles[i] != NULL; i++) {
            gchar *label_text;
            gchar *icon_name = NULL;
            
            if (plugin->profile_settings) {
                GHashTableIter iter;
                gpointer key, value;
                g_hash_table_iter_init(&iter, plugin->profile_settings);
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
            }
            
            label_text = g_strdup(profiles[i]);
            
            item = gtk_check_menu_item_new();
            if (!item) {
                g_free(label_text);
                if (icon_name) g_free(icon_name);
                continue;
            }
            
            hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            
            if (icon_name) {
                image = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
                gtk_box_pack_start(GTK_BOX(hbox), image, FALSE, FALSE, 0);
                g_free(icon_name);
            }
            
            label_widget = gtk_label_new(label_text);
            gtk_box_pack_start(GTK_BOX(hbox), label_widget, FALSE, FALSE, 0);
            
            gtk_container_add(GTK_CONTAINER(item), hbox);
            gtk_widget_show_all(hbox);
            
            if (g_strcmp0(profiles[i], current) == 0) {
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), TRUE);
                gtk_widget_set_sensitive(item, FALSE);
            } else {
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), FALSE);
            }
            
            g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(on_profile_selected), plugin);
            g_object_set_data_full(G_OBJECT(item), "profile", g_strdup(profiles[i]), g_free);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
            g_free(label_text);
        }
    }

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_widget(GTK_MENU(menu), plugin->button, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH, NULL);
}

static void on_profile_selected(GtkMenuItem *item, AsusdBatteryPlugin *plugin) {
    gchar *profile;
    gchar *display_name = NULL;
    
    if (!item || !plugin || !plugin->asusd_available) return;
    
    profile = (gchar *)g_object_get_data(G_OBJECT(item), "profile");
    if (!profile) return;

    if (asusd_set_profile(plugin, profile)) {
        gchar *new_profile = NULL;
        if (asusd_get_current_profile(plugin, &new_profile)) {
            g_free(plugin->current_profile);
            plugin->current_profile = new_profile;
        }
        
        display_name = g_strdup(profile);
        send_notification(_("Performance profile changed"),
                         g_strdup_printf(_("Current profile: %s"), display_name),
                         FALSE, "battery-good-symbolic");
        g_free(display_name);
        update_profile_display(plugin);
    } else {
        send_notification(_("Error changing profile"),
                         _("Failed to set profile via ASUSD"),
                         TRUE, "emblem-readonly");
    }
}

static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, AsusdBatteryPlugin *plugin) {
    GtkWidget *menu;
    GtkWidget *item;
    
    if (!plugin || !widget) return FALSE;
    
    if (event->button == 3) {
        menu = gtk_menu_new();
        if (!menu) return FALSE;
        
        item = gtk_menu_item_new_with_label(_("Settings"));
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

static void create_about_dialog(AsusdBatteryPlugin *plugin) {
    GtkWidget *dialog;
    GtkWidget *content_area;
    GtkWidget *vbox;
    GtkWidget *label;
    GtkWidget *version_label;
    GtkWidget *authors_label;
    GtkWidget *description_label;
    GtkWidget *separator;
    GtkWidget *close_button;
    
    if (!plugin) return;
    
    dialog = gtk_dialog_new_with_buttons(_("About ASUS Battery Plugin"),
                                        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(plugin->plugin))),
                                        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                        NULL,
                                        NULL);
    
    if (!dialog) {
        g_warning("Failed to create about dialog");
        return;
    }
    
    /* Устанавливаем иконку для окна About */
    gtk_window_set_icon_name(GTK_WINDOW(dialog), "dialog-information");
    
    gtk_window_set_default_size(GTK_WINDOW(dialog), 400, 280);
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    
    content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    if (!content_area) {
        gtk_widget_destroy(dialog);
        return;
    }
    
    gtk_container_set_border_width(GTK_CONTAINER(content_area), 20);
    
    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    if (!vbox) {
        gtk_widget_destroy(dialog);
        return;
    }
    gtk_container_add(GTK_CONTAINER(content_area), vbox);
    
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), _("<b>ASUS Battery</b>"));
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);
    
    version_label = gtk_label_new(_("Version 1.0"));
    gtk_box_pack_start(GTK_BOX(vbox), version_label, FALSE, FALSE, 0);
    
    separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), separator, FALSE, FALSE, 5);
    
    description_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(description_label), 
                         _("<small>Plugin for managing ASUS laptop power profiles via asusd.\n"
                           "Allows switching between performance, balanced and quiet modes.</small>"));
    gtk_box_pack_start(GTK_BOX(vbox), description_label, FALSE, FALSE, 0);
    
    authors_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(authors_label),
                         _("<small><b>Authors:</b> Deepseek and korn3r</small>"));
    gtk_box_pack_start(GTK_BOX(vbox), authors_label, FALSE, FALSE, 0);
    
    close_button = gtk_button_new_with_label(_("Close"));
    if (close_button) {
        g_signal_connect_swapped(G_OBJECT(close_button), "clicked", 
                                 G_CALLBACK(gtk_widget_destroy), dialog);
        gtk_box_pack_start(GTK_BOX(vbox), close_button, FALSE, FALSE, 10);
        gtk_widget_set_halign(close_button, GTK_ALIGN_CENTER);
    }
    
    gtk_widget_show_all(dialog);
}

static void on_one_shot_clicked(GtkButton *button, GtkWidget *dialog) {
    GtkWidget *message_dialog;
    gint response;
    
    /* Создаем диалог подтверждения */
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
        /* Получаем соединение и вызываем метод OneShotFullCharge */
        GDBusConnection *connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
        if (connection) {
            GError *error = NULL;
            
            /* Вызываем метод OneShotFullCharge */
            GVariant *result = g_dbus_connection_call_sync(
                connection,
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
                g_warning("Failed to call OneShotFullCharge: %s", error->message);
                g_error_free(error);
                
                GtkWidget *error_dialog = gtk_message_dialog_new(
                    GTK_WINDOW(dialog),
                    GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                    GTK_MESSAGE_ERROR,
                    GTK_BUTTONS_OK,
                    _("Failed to start one-shot full charge:\n%s"),
                    error->message
                );
                gtk_window_set_icon_name(GTK_WINDOW(error_dialog), NULL);
                gtk_dialog_run(GTK_DIALOG(error_dialog));
                gtk_widget_destroy(error_dialog);
            } else {
                if (result) {
                    g_variant_unref(result);
                }
                
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
            
            g_object_unref(connection);
        }
    }
}

static void create_settings_dialog(AsusdBatteryPlugin *plugin) {
    GtkWidget *dialog;
    GtkWidget *content_area;
    GtkWidget *vbox;
    GtkWidget *checkbutton;
    GtkWidget *hide_icon_check_widget;
    GtkWidget *hide_text_check_widget;
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
    gchar **profiles;
    GtkWidget *auto_frame;
    GtkWidget *auto_vbox;
    GtkWidget *hide_frame;
    GtkWidget *hide_vbox;
    GtkWidget *battery_hbox;
    GtkWidget *battery_vbox;
    GtkWidget *ac_hbox;
    GtkWidget *one_shot_hbox;
    GtkWidget *one_shot_label;
    GtkWidget *one_shot_button;
    int row = 0;
    int last_row = 0;
    
    if (!plugin) return;
    
    if (plugin->asusd_available) {
        asusd_sync_all_settings(plugin);
    }
    
    dialog = gtk_dialog_new_with_buttons(_("Power Profile Settings"),
                                        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(plugin->plugin))),
                                        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                        _("_Close"), GTK_RESPONSE_CLOSE,
                                        NULL);
    
    if (!dialog) {
        g_warning("Failed to create settings dialog");
        return;
    }
    
    /* Устанавливаем иконку для окна настроек */
    gtk_window_set_icon_name(GTK_WINDOW(dialog), "emblem-system");
    
    /* Делаем окно без возможности изменения размера */
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    
    content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    if (!content_area) {
        gtk_widget_destroy(dialog);
        return;
    }
    
    gtk_container_set_border_width(GTK_CONTAINER(content_area), 10);
    
    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    if (!vbox) {
        gtk_widget_destroy(dialog);
        return;
    }
    gtk_container_add(GTK_CONTAINER(content_area), vbox);
    
    /* Основной контейнер с выравниванием по центру */
    GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_halign(main_vbox, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), main_vbox, TRUE, TRUE, 0);
    
    /* ========== Auto switch profiles ========== */
    auto_frame = gtk_frame_new(_("Auto switch profiles"));
    gtk_box_pack_start(GTK_BOX(main_vbox), auto_frame, FALSE, FALSE, 0);
    
    auto_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(auto_vbox), 5);
    gtk_container_add(GTK_CONTAINER(auto_frame), auto_vbox);
    
    /* Горизонтальный контейнер для AC и Battery */
    ac_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 20);
    gtk_widget_set_halign(ac_hbox, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(auto_vbox), ac_hbox, FALSE, FALSE, 0);
    
    /* On AC - слева */
    frame_ac = gtk_frame_new(_("On AC"));
    gtk_box_pack_start(GTK_BOX(ac_hbox), frame_ac, FALSE, FALSE, 0);
    
    hbox_ac = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(hbox_ac), 5);
    gtk_container_add(GTK_CONTAINER(frame_ac), hbox_ac);
    
    GtkWidget *check_ac = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check_ac), plugin->auto_switch_ac_enabled);
    if (!plugin->asusd_available) {
        gtk_widget_set_sensitive(check_ac, FALSE);
    }
    g_object_set_data(G_OBJECT(dialog), "check_ac", check_ac);
    gtk_box_pack_start(GTK_BOX(hbox_ac), check_ac, FALSE, FALSE, 0);
    
    store_ac = gtk_list_store_new(1, G_TYPE_STRING);
    combo_ac = gtk_combo_box_new_with_model(GTK_TREE_MODEL(store_ac));
    renderer = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combo_ac), renderer, TRUE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(combo_ac), renderer, "text", 0, NULL);
    gtk_widget_set_size_request(combo_ac, 100, -1);
    
    profiles = asusd_get_available_profiles(plugin);
    if (profiles) {
        GtkTreeIter iter;
        int active_index = 0;
        for (int i = 0; profiles[i] != NULL; i++) {
            gtk_list_store_append(store_ac, &iter);
            gtk_list_store_set(store_ac, &iter, 0, profiles[i], -1);
            if (plugin->auto_switch_ac_profile && g_strcmp0(profiles[i], plugin->auto_switch_ac_profile) == 0) {
                active_index = i;
            }
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo_ac), active_index);
        g_strfreev(profiles);
    }
    gtk_widget_set_sensitive(combo_ac, plugin->auto_switch_ac_enabled && plugin->asusd_available);
    g_object_set_data(G_OBJECT(dialog), "combo_ac", combo_ac);
    gtk_box_pack_start(GTK_BOX(hbox_ac), combo_ac, FALSE, FALSE, 0);
    
    g_signal_connect(G_OBJECT(check_ac), "toggled", G_CALLBACK(on_auto_switch_toggled), dialog);
    
    /* On Battery - справа */
    frame_battery = gtk_frame_new(_("On Battery"));
    gtk_box_pack_start(GTK_BOX(ac_hbox), frame_battery, FALSE, FALSE, 0);
    
    hbox_battery = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(hbox_battery), 5);
    gtk_container_add(GTK_CONTAINER(frame_battery), hbox_battery);
    
    GtkWidget *check_battery = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check_battery), plugin->auto_switch_battery_enabled);
    if (!plugin->asusd_available) {
        gtk_widget_set_sensitive(check_battery, FALSE);
    }
    g_object_set_data(G_OBJECT(dialog), "check_battery", check_battery);
    gtk_box_pack_start(GTK_BOX(hbox_battery), check_battery, FALSE, FALSE, 0);
    
    store_battery = gtk_list_store_new(1, G_TYPE_STRING);
    combo_battery = gtk_combo_box_new_with_model(GTK_TREE_MODEL(store_battery));
    renderer = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combo_battery), renderer, TRUE);
    gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(combo_battery), renderer, "text", 0, NULL);
    gtk_widget_set_size_request(combo_battery, 100, -1);
    
    profiles = asusd_get_available_profiles(plugin);
    if (profiles) {
        GtkTreeIter iter;
        int active_index = 0;
        for (int i = 0; profiles[i] != NULL; i++) {
            gtk_list_store_append(store_battery, &iter);
            gtk_list_store_set(store_battery, &iter, 0, profiles[i], -1);
            if (plugin->auto_switch_battery_profile && g_strcmp0(profiles[i], plugin->auto_switch_battery_profile) == 0) {
                active_index = i;
            }
        }
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo_battery), active_index);
        g_strfreev(profiles);
    }
    gtk_widget_set_sensitive(combo_battery, plugin->auto_switch_battery_enabled && plugin->asusd_available);
    g_object_set_data(G_OBJECT(dialog), "combo_battery", combo_battery);
    gtk_box_pack_start(GTK_BOX(hbox_battery), combo_battery, FALSE, FALSE, 0);
    
    g_signal_connect(G_OBJECT(check_battery), "toggled", G_CALLBACK(on_auto_switch_toggled), dialog);
    
    /* ========== Profile names and icons ========== */
    hide_frame = gtk_frame_new(_("Profile names and icons"));
    gtk_box_pack_start(GTK_BOX(main_vbox), hide_frame, FALSE, FALSE, 0);
    
    hide_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_set_border_width(GTK_CONTAINER(hide_vbox), 5);
    gtk_container_add(GTK_CONTAINER(hide_frame), hide_vbox);
    gtk_widget_set_halign(hide_vbox, GTK_ALIGN_CENTER);
    
    /* Сетка с настройками имен и иконок */
    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_box_pack_start(GTK_BOX(hide_vbox), grid, FALSE, FALSE, 0);
    
    row = 0;
    
    if (plugin->profile_order) {
        GList *iter;
        for (iter = plugin->profile_order; iter != NULL; iter = iter->next) {
            guint32 enum_val = GPOINTER_TO_UINT(iter->data);
            ProfileSettings *settings = g_hash_table_lookup(plugin->profile_settings, 
                                                            g_strdup_printf("%d", enum_val));
            if (!settings) continue;
            
            /* Название профиля - xalign=0 (выравнивание по левому краю) */
            gchar *display_name = g_strdup(settings->default_name);
            if (display_name[0] >= 'a' && display_name[0] <= 'z') {
                display_name[0] = g_ascii_toupper(display_name[0]);
            }
            label = gtk_label_new(display_name);
            gtk_label_set_xalign(GTK_LABEL(label), 0.0);
            gtk_widget_set_size_request(label, 80, -1);
            gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
            g_free(display_name);
            
            /* Поле ввода текста */
            entry_name = gtk_entry_new();
            if (settings->name && strlen(settings->name) > 0) {
                gtk_entry_set_text(GTK_ENTRY(entry_name), settings->name);
            }
            gtk_entry_set_placeholder_text(GTK_ENTRY(entry_name), _("Custom name"));
            gtk_widget_set_size_request(entry_name, 120, -1);
            gtk_entry_set_max_length(GTK_ENTRY(entry_name), 20);
            g_object_set_data(G_OBJECT(dialog), g_strdup_printf("profile_name_%d", enum_val), entry_name);
            gtk_grid_attach(GTK_GRID(grid), entry_name, 1, row, 1, 1);
            
            /* Поле ввода иконки */
            entry_icon = gtk_entry_new();
            if (settings->icon && strlen(settings->icon) > 0) {
                gtk_entry_set_text(GTK_ENTRY(entry_icon), settings->icon);
            }
            gtk_entry_set_placeholder_text(GTK_ENTRY(entry_icon), _("Icon name"));
            gtk_widget_set_size_request(entry_icon, 120, -1);
            g_object_set_data(G_OBJECT(dialog), g_strdup_printf("profile_icon_%d", enum_val), entry_icon);
            gtk_grid_attach(GTK_GRID(grid), entry_icon, 2, row, 1, 1);
            
            row++;
        }
    }
    
    last_row = row;
    
    /* Добавляем чекбоксы в конец сетки */
    /* Hide text - под полем ввода Custom name (колонка 1) */
    hide_text_check_widget = gtk_check_button_new_with_label(_("Hide text"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hide_text_check_widget), plugin->hide_text);
    g_object_set_data(G_OBJECT(dialog), "hide_text_check", hide_text_check_widget);
    gtk_grid_attach(GTK_GRID(grid), hide_text_check_widget, 1, last_row, 1, 1);
    
    /* Hide icon - под полем ввода Icon name (колонка 2) */
    hide_icon_check_widget = gtk_check_button_new_with_label(_("Hide icon"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hide_icon_check_widget), plugin->hide_icon);
    g_object_set_data(G_OBJECT(dialog), "hide_icon_check", hide_icon_check_widget);
    gtk_grid_attach(GTK_GRID(grid), hide_icon_check_widget, 2, last_row, 1, 1);
    
    g_signal_connect(G_OBJECT(hide_icon_check_widget), "toggled", G_CALLBACK(on_hide_toggle), dialog);
    g_signal_connect(G_OBJECT(hide_text_check_widget), "toggled", G_CALLBACK(on_hide_toggle), dialog);
    
    /* ========== Battery limit и One-shot full charge ========== */
    battery_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), battery_vbox, FALSE, FALSE, 0);
    
    /* Первая строка - Limit battery charge to 80% */
    battery_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(battery_hbox, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(battery_vbox), battery_hbox, FALSE, FALSE, 0);
    
    checkbutton = gtk_check_button_new_with_label(_("Limit battery charge to 80%"));
    if (!checkbutton) {
        gtk_widget_destroy(dialog);
        return;
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkbutton), plugin->battery_limit_enabled);
    if (!plugin->asusd_available) {
        gtk_widget_set_sensitive(checkbutton, FALSE);
        gtk_widget_set_tooltip_text(checkbutton, _("ASUSD not available"));
    }
    g_object_set_data(G_OBJECT(dialog), "battery_check", checkbutton);
    
    /* Отступ слева 17px */
    GtkWidget *battery_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(battery_spacer, 17, -1);
    gtk_box_pack_start(GTK_BOX(battery_hbox), battery_spacer, FALSE, FALSE, 0);
    
    gtk_box_pack_start(GTK_BOX(battery_hbox), checkbutton, FALSE, FALSE, 0);
    
    GtkWidget *battery_spacer_right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(battery_hbox), battery_spacer_right, TRUE, TRUE, 0);
    
    /* Вторая строка - One-shot full charge */
    one_shot_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_halign(one_shot_hbox, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(battery_vbox), one_shot_hbox, FALSE, FALSE, 0);
    
    /* Отступ слева 17px */
    GtkWidget *one_shot_spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_size_request(one_shot_spacer, 17, -1);
    gtk_box_pack_start(GTK_BOX(one_shot_hbox), one_shot_spacer, FALSE, FALSE, 0);
    
    /* Текст "One-shot full charge" */
    one_shot_label = gtk_label_new(_("One-shot full charge"));
    gtk_widget_set_halign(one_shot_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(one_shot_hbox), one_shot_label, FALSE, FALSE, 0);
    
    /* Кнопка "Start" */
    one_shot_button = gtk_button_new_with_label(_("Start"));
    gtk_widget_set_sensitive(one_shot_button, plugin->asusd_available);
    gtk_widget_set_tooltip_text(one_shot_button, _("Charge battery to 100% once, ignoring the 80% limit"));
    g_object_set_data(G_OBJECT(dialog), "one_shot_button", one_shot_button);
    gtk_box_pack_start(GTK_BOX(one_shot_hbox), one_shot_button, FALSE, FALSE, 0);
    
    /* Подключаем сигнал для кнопки One-shot full charge */
    g_signal_connect(G_OBJECT(one_shot_button), "clicked", G_CALLBACK(on_one_shot_clicked), dialog);
    
    GtkWidget *one_shot_spacer_right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(one_shot_hbox), one_shot_spacer_right, TRUE, TRUE, 0);
    
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(on_settings_response), plugin);
    gtk_widget_show_all(dialog);
    
    /* Устанавливаем размер окна по содержимому */
    gtk_window_set_default_size(GTK_WINDOW(dialog), -1, -1);
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

static void on_settings_response(GtkDialog *dialog, gint response_id, AsusdBatteryPlugin *plugin) {
    GtkWidget *widget;
    gboolean settings_changed = FALSE;
    
    if (!dialog || !plugin) return;
    
    if (response_id == GTK_RESPONSE_CLOSE || response_id == GTK_RESPONSE_DELETE_EVENT) {
        widget = g_object_get_data(G_OBJECT(dialog), "battery_check");
        if (widget && plugin->asusd_available) {
            gboolean new_val = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
            if (new_val != plugin->battery_limit_enabled) {
                plugin->battery_limit_enabled = new_val;
                settings_changed = TRUE;

                guint32 limit = new_val ? 80 : 100;
                asusd_set_battery_limit(plugin, limit);
                plugin->current_battery_limit = limit;
            }
        }
 
        widget = g_object_get_data(G_OBJECT(dialog), "check_ac");
        if (widget && plugin->asusd_available) {
            gboolean new_val = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
            if (new_val != plugin->auto_switch_ac_enabled) {
                plugin->auto_switch_ac_enabled = new_val;
                settings_changed = TRUE;
                asusd_set_bool_property(plugin, "ChangePlatformProfileOnAc", new_val);
            }
        }
        
        widget = g_object_get_data(G_OBJECT(dialog), "combo_ac");
        if (widget && plugin->asusd_available) {
            GtkTreeIter iter;
            if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(widget), &iter)) {
                GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(widget));
                gchar *value;
                gtk_tree_model_get(model, &iter, 0, &value, -1);
                if (value) {
                    if (g_strcmp0(value, plugin->auto_switch_ac_profile) != 0) {
                        g_free(plugin->auto_switch_ac_profile);
                        plugin->auto_switch_ac_profile = g_strdup(value);
                        settings_changed = TRUE;
                        
                        guint32 enum_val = asusd_find_enum_by_name(plugin, value);
                        if (enum_val != 999) {
                            asusd_set_uint32_property(plugin, "PlatformProfileOnAc", enum_val);
                        }
                    }
                    g_free(value);
                }
            }
        }
        
        widget = g_object_get_data(G_OBJECT(dialog), "check_battery");
        if (widget && plugin->asusd_available) {
            gboolean new_val = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
            if (new_val != plugin->auto_switch_battery_enabled) {
                plugin->auto_switch_battery_enabled = new_val;
                settings_changed = TRUE;
                asusd_set_bool_property(plugin, "ChangePlatformProfileOnBattery", new_val);
            }
        }
        
        widget = g_object_get_data(G_OBJECT(dialog), "combo_battery");
        if (widget && plugin->asusd_available) {
            GtkTreeIter iter;
            if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(widget), &iter)) {
                GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(widget));
                gchar *value;
                gtk_tree_model_get(model, &iter, 0, &value, -1);
                if (value) {
                    if (g_strcmp0(value, plugin->auto_switch_battery_profile) != 0) {
                        g_free(plugin->auto_switch_battery_profile);
                        plugin->auto_switch_battery_profile = g_strdup(value);
                        settings_changed = TRUE;
                        
                        guint32 enum_val = asusd_find_enum_by_name(plugin, value);
                        if (enum_val != 999) {
                            asusd_set_uint32_property(plugin, "PlatformProfileOnBattery", enum_val);
                        }
                    }
                    g_free(value);
                }
            }
        }
        
        widget = g_object_get_data(G_OBJECT(dialog), "hide_icon_check");
        if (widget) {
            gboolean new_val = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
            if (new_val != plugin->hide_icon) {
                plugin->hide_icon = new_val;
                settings_changed = TRUE;
            }
        }
        
        widget = g_object_get_data(G_OBJECT(dialog), "hide_text_check");
        if (widget) {
            gboolean new_val = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
            if (new_val != plugin->hide_text) {
                plugin->hide_text = new_val;
                settings_changed = TRUE;
            }
        }
        
        if (plugin->hide_icon && plugin->hide_text) {
            plugin->hide_icon = FALSE;
            settings_changed = TRUE;
        }
        
        if (plugin->profile_settings) {
            GHashTableIter iter;
            gpointer key, value;
            g_hash_table_iter_init(&iter, plugin->profile_settings);
            while (g_hash_table_iter_next(&iter, &key, &value)) {
                ProfileSettings *settings = (ProfileSettings*)value;
                
                gchar *key_name = g_strdup_printf("profile_name_%d", settings->enum_value);
                widget = g_object_get_data(G_OBJECT(dialog), key_name);
                if (widget) {
                    const gchar *text = gtk_entry_get_text(GTK_ENTRY(widget));
                    gchar *new_text = (text && strlen(text) > 0) ? g_strdup(text) : NULL;
                    if (g_strcmp0(new_text, settings->name) != 0) {
                        g_free(settings->name);
                        settings->name = new_text;
                        settings_changed = TRUE;
                    } else {
                        g_free(new_text);
                    }
                }
                g_free(key_name);
                
                gchar *key_icon = g_strdup_printf("profile_icon_%d", settings->enum_value);
                widget = g_object_get_data(G_OBJECT(dialog), key_icon);
                if (widget) {
                    const gchar *text = gtk_entry_get_text(GTK_ENTRY(widget));
                    gchar *new_text = (text && strlen(text) > 0) ? g_strdup(text) : NULL;
                    
                    if (new_text) {
                        GtkIconTheme *theme = gtk_icon_theme_get_default();
                        GtkIconInfo *icon_info = gtk_icon_theme_lookup_icon(theme, new_text, 16, GTK_ICON_LOOKUP_GENERIC_FALLBACK);
                        if (!icon_info) {
                            g_warning("Icon '%s' not found, using default", new_text);
                            g_free(new_text);
                            new_text = NULL;
                        } else {
                            g_object_unref(icon_info);
                        }
                    }
                    
                    if (g_strcmp0(new_text, settings->icon) != 0) {
                        g_free(settings->icon);
                        settings->icon = new_text;
                        settings_changed = TRUE;
                    } else {
                        g_free(new_text);
                    }
                }
                g_free(key_icon);
            }
        }
        
        if (settings_changed) {
            save_settings(plugin);
            if (plugin->asusd_available) {
                asusd_sync_all_settings(plugin);
            }
            update_profile_display(plugin);
        }
        
        gtk_widget_destroy(GTK_WIDGET(dialog));
    }
}

static void on_hide_toggle(GtkToggleButton *toggle_button, GtkWidget *dialog) {
    GtkWidget *hide_icon_check;
    GtkWidget *hide_text_check;
    gboolean icon_active, text_active;
    
    if (!dialog) return;
    
    hide_icon_check = g_object_get_data(G_OBJECT(dialog), "hide_icon_check");
    hide_text_check = g_object_get_data(G_OBJECT(dialog), "hide_text_check");
    
    if (!hide_icon_check || !hide_text_check) return;
    
    icon_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(hide_icon_check));
    text_active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(hide_text_check));
    
    if (icon_active && text_active) {
        if (toggle_button == GTK_TOGGLE_BUTTON(hide_icon_check)) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hide_text_check), FALSE);
        } else {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hide_icon_check), FALSE);
        }
    }
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);
    return 0;
}
