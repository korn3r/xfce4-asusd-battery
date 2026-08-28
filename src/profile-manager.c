#include "profile-manager.h"
#include "utils.h"
#include "plugin.h"
#include <string.h>
#include "debug.h"

ProfileSettings* profile_settings_new(guint32 enum_value, const gchar *default_name) {
    ProfileSettings *settings = g_new0(ProfileSettings, 1);
    settings->enum_value = enum_value;
    settings->default_name = g_strdup(default_name ? default_name : "unknown");
    settings->name = NULL;
    settings->icon = NULL;
    return settings;
}

void profile_settings_free(ProfileSettings *settings) {
    if (!settings) return;
    g_free(settings->name);
    g_free(settings->icon);
    g_free(settings->default_name);
    g_free(settings);
}

const gchar* profile_name_from_enum(AsusdBatteryPlugin *plugin, guint32 enum_value) {
    if (!plugin) return "balanced";
    
    if (!plugin->profile_lookup) {
        DEBUG_DEBUG("profile_name_from_enum: profile_lookup is NULL, using fallback");
        return asusd_enum_to_default_name(enum_value);
    }
    
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

gboolean profile_enum_from_name(AsusdBatteryPlugin *plugin, const gchar *name, guint32 *enum_value) {
    if (!plugin || !name || !enum_value) return FALSE;
    
    if (!plugin->profile_lookup) {
        DEBUG_DEBUG("profile_enum_from_name: profile_lookup is NULL");
        return FALSE;
    }
    
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

void create_fallback_profiles(AsusdBatteryPlugin *plugin) {
    if (!plugin || plugin->is_disposing) return;
    
    if (plugin->profiles && plugin->profiles->len > 0) {
        DEBUG_DEBUG("ASUSD: Profiles already exist (%d), not creating fallback", plugin->profiles->len);
        return;
    }
    
    DEBUG_DEBUG("ASUSD: Creating fallback profiles");
    
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
    
    const char *default_profiles[] = {"balanced", "performance", "quiet"};
    for (int i = 0; i < 3; i++) {
        ProfileSettings *settings = profile_settings_new(i, default_profiles[i]);
        
        gchar *key = g_strdup_printf("%s/profile_%d_name", CONFIG_PROPERTY_PREFIX, i);
        gchar *saved_name = xfconf_channel_get_string(channel, key, NULL);
        if (saved_name && strlen(saved_name) > 0) {
            g_free(settings->name);
            settings->name = saved_name;
        } else {
            g_free(saved_name);
        }
        g_free(key);
        
        key = g_strdup_printf("%s/profile_%d_icon", CONFIG_PROPERTY_PREFIX, i);
        gchar *saved_icon = xfconf_channel_get_string(channel, key, NULL);
        if (saved_icon && strlen(saved_icon) > 0) {
            g_free(settings->icon);
            settings->icon = saved_icon;
        } else {
            g_free(saved_icon);
        }
        g_free(key);
        
        g_ptr_array_add(plugin->profiles, settings);
        g_hash_table_insert(plugin->profile_lookup, GUINT_TO_POINTER(i), settings);
        DEBUG_DEBUG("ASUSD: Added fallback profile: %s (enum: %d)", default_profiles[i], i);
    }
}

void parse_profile_choices(AsusdBatteryPlugin *plugin, GVariant *value) {
    if (!plugin || !value || plugin->is_disposing) return;
    
    if (!plugin->profiles) {
        plugin->profiles = g_ptr_array_new_with_free_func((GDestroyNotify)profile_settings_free);
    }
    if (!plugin->profile_lookup) {
        plugin->profile_lookup = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);
    }
    
    XfconfChannel *channel = xfconf_channel_get(CONFIG_CHANNEL);
    GVariantIter iter;
    guint32 enum_value;
    g_variant_iter_init(&iter, value);
    while (g_variant_iter_next(&iter, "u", &enum_value)) {
        const char *default_name = asusd_enum_to_default_name(enum_value);
        ProfileSettings *settings = profile_settings_new(enum_value, default_name);
        gchar *key = g_strdup_printf("%s/profile_%d_name", CONFIG_PROPERTY_PREFIX, enum_value);
        gchar *saved_name = xfconf_channel_get_string(channel, key, NULL);
        if (saved_name && strlen(saved_name) > 0) settings->name = saved_name;
        else g_free(saved_name);
        g_free(key);
        key = g_strdup_printf("%s/profile_%d_icon", CONFIG_PROPERTY_PREFIX, enum_value);
        gchar *saved_icon = xfconf_channel_get_string(channel, key, NULL);
        if (saved_icon && strlen(saved_icon) > 0) settings->icon = saved_icon;
        else g_free(saved_icon);
        g_free(key);
        g_ptr_array_add(plugin->profiles, settings);
        g_hash_table_insert(plugin->profile_lookup, GUINT_TO_POINTER(enum_value), settings);
        DEBUG_DEBUG("ASUSD: Loaded profile: %s (enum: %u)", default_name, enum_value);
    }
}

gchar** asusd_get_available_profiles(AsusdBatteryPlugin *plugin) {
    if (!plugin) return NULL;
    
    if (!plugin->profiles || plugin->profiles->len == 0) {
        DEBUG_DEBUG("asusd_get_available_profiles: no profiles loaded, creating fallback");
        create_fallback_profiles(plugin);
    }
    
    if (!plugin->profiles || plugin->profiles->len == 0) {
        DEBUG_DEBUG("asusd_get_available_profiles: still no profiles, returning empty");
        gchar **empty = g_new0(gchar*, 1);
        return empty;
    }
    
    GPtrArray *result = g_ptr_array_new();
    for (guint i = 0; i < plugin->profiles->len; i++) {
        ProfileSettings *settings = g_ptr_array_index(plugin->profiles, i);
        const char *display_name = (settings->name && strlen(settings->name) > 0) ? settings->name : settings->default_name;
        if (display_name) g_ptr_array_add(result, g_strdup(display_name));
    }
    g_ptr_array_add(result, NULL);
    return (gchar**)g_ptr_array_free(result, FALSE);
}
