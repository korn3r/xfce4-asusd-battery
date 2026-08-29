/* src/profile-manager.c */
#include "profile-manager.h"
#include "utils.h"
#include "plugin.h"
#include "debug.h"

/* ========== Вспомогательная функция ========== */

static const char* enum_to_default_name(guint32 enum_value) {
    switch (enum_value) {
        case 0: return "balanced";
        case 1: return "performance";
        case 2: return "quiet";
        case 3: return "powersave";
        default: return "balanced";
    }
}

/* ========== Управление профилями ========== */

void profile_settings_free(ProfileSettings *settings) {
    if (!settings) return;
    g_free(settings->default_name);
    g_free(settings->name);
    g_free(settings->icon);
    g_free(settings);
}

const gchar* profile_name_from_enum(AsusdBatteryPlugin *plugin, guint32 enum_val) {
    if (!plugin) return "balanced";
    
    if (plugin->profile_lookup) {
        ProfileSettings *settings = g_hash_table_lookup(plugin->profile_lookup, GINT_TO_POINTER(enum_val));
        if (settings && settings->name && strlen(settings->name) > 0) {
            return settings->name;
        }
        if (settings && settings->default_name) {
            return settings->default_name;
        }
    }
    
    return enum_to_default_name(enum_val);
}

gboolean profile_enum_from_name(AsusdBatteryPlugin *plugin, const gchar *name, guint32 *enum_val) {
    if (!plugin || !name || !enum_val) return FALSE;
    
    if (plugin->profile_lookup) {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, plugin->profile_lookup);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            ProfileSettings *settings = (ProfileSettings*)value;
            if (settings->name && g_strcmp0(name, settings->name) == 0) {
                *enum_val = settings->enum_value;
                return TRUE;
            }
            if (settings->default_name && g_strcmp0(name, settings->default_name) == 0) {
                *enum_val = settings->enum_value;
                return TRUE;
            }
        }
    }
    
    for (guint32 i = 0; i < 10; i++) {
        const char *default_name = enum_to_default_name(i);
        if (default_name && g_strcmp0(name, default_name) == 0) {
            *enum_val = i;
            return TRUE;
        }
    }
    
    return FALSE;
}

const gchar* get_profile_icon(AsusdBatteryPlugin *plugin, const gchar *profile_name) {
    if (!plugin || !profile_name) return "battery-good-symbolic";
    
    if (plugin->profile_lookup) {
        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init(&iter, plugin->profile_lookup);
        while (g_hash_table_iter_next(&iter, &key, &value)) {
            ProfileSettings *settings = (ProfileSettings*)value;
            if (settings->name && g_strcmp0(profile_name, settings->name) == 0) {
                if (settings->icon && strlen(settings->icon) > 0) {
                    return settings->icon;
                }
                break;
            }
            if (settings->default_name && g_strcmp0(profile_name, settings->default_name) == 0) {
                if (settings->icon && strlen(settings->icon) > 0) {
                    return settings->icon;
                }
                break;
            }
        }
    }
    
    if (g_strcmp0(profile_name, "performance") == 0) return "battery-full-symbolic";
    if (g_strcmp0(profile_name, "balanced") == 0) return "battery-good-symbolic";
    if (g_strcmp0(profile_name, "quiet") == 0) return "battery-low-symbolic";
    
    return "battery-good-symbolic";
}

gchar** asusd_get_available_profiles(AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->profiles) {
        gchar **result = g_new0(gchar*, 4);
        result[0] = g_strdup("balanced");
        result[1] = g_strdup("performance");
        result[2] = g_strdup("quiet");
        result[3] = NULL;
        return result;
    }
    
    GPtrArray *names = g_ptr_array_new();
    for (guint i = 0; i < plugin->profiles->len; i++) {
        ProfileSettings *settings = g_ptr_array_index(plugin->profiles, i);
        const char *name = settings->name && strlen(settings->name) > 0 ? settings->name : settings->default_name;
        if (name) g_ptr_array_add(names, g_strdup(name));
    }
    g_ptr_array_add(names, NULL);
    
    return (gchar**)g_ptr_array_free(names, FALSE);
}

void create_fallback_profiles(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    DEBUG_TRACE("ASUSD: Creating fallback profiles");
    
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
    
    const struct {
        guint32 enum_val;
        const char *name;
        const char *icon;
    } fallback_profiles[] = {
        {0, "balanced", "battery-good-symbolic"},
        {1, "performance", "battery-full-symbolic"},
        {2, "quiet", "battery-low-symbolic"},
        {3, "powersave", "battery-caution-symbolic"}
    };
    
    for (guint i = 0; i < G_N_ELEMENTS(fallback_profiles); i++) {
        ProfileSettings *settings = g_new0(ProfileSettings, 1);
        settings->enum_value = fallback_profiles[i].enum_val;
        settings->default_name = g_strdup(fallback_profiles[i].name);
        settings->name = NULL;
        settings->icon = g_strdup(fallback_profiles[i].icon);
        
        g_ptr_array_add(plugin->profiles, settings);
        g_hash_table_insert(plugin->profile_lookup, GINT_TO_POINTER(settings->enum_value), settings);
        DEBUG_TRACE("ASUSD: Added fallback profile: %s (enum: %u)", settings->default_name, settings->enum_value);
    }
}

void parse_profile_choices(AsusdBatteryPlugin *plugin, GVariant *value) {
    if (!plugin || !value) return;
    
    GVariantIter iter;
    g_variant_iter_init(&iter, value);
    guint32 enum_value;
    
    while (g_variant_iter_next(&iter, "u", &enum_value)) {
        const char *default_name = enum_to_default_name(enum_value);
        ProfileSettings *settings = g_new0(ProfileSettings, 1);
        settings->enum_value = enum_value;
        settings->default_name = g_strdup(default_name);
        settings->name = NULL;
        settings->icon = g_strdup("battery-good-symbolic");
        
        g_ptr_array_add(plugin->profiles, settings);
        g_hash_table_insert(plugin->profile_lookup, GINT_TO_POINTER(enum_value), settings);
        DEBUG_TRACE("ASUSD: Loaded profile: %s (enum: %u)", default_name, enum_value);
    }
}
