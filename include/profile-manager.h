/* include/profile-manager.h */
#ifndef PROFILE_MANAGER_H
#define PROFILE_MANAGER_H

#include <glib.h>
#include "plugin.h"

/* ========== ProfileSettings структура ========== */

typedef struct _ProfileSettings {
    guint32 enum_value;
    char *default_name;
    char *name;
    char *icon;
} ProfileSettings;

/* ========== Управление профилями ========== */

void create_fallback_profiles(AsusdBatteryPlugin *plugin);
void parse_profile_choices(AsusdBatteryPlugin *plugin, GVariant *value);
const gchar* profile_name_from_enum(AsusdBatteryPlugin *plugin, guint32 enum_val);
gboolean profile_enum_from_name(AsusdBatteryPlugin *plugin, const gchar *name, guint32 *enum_val);
const gchar* get_profile_icon(AsusdBatteryPlugin *plugin, const gchar *profile_name);
gchar** asusd_get_available_profiles(AsusdBatteryPlugin *plugin);
void profile_settings_free(ProfileSettings *settings);

#endif /* PROFILE_MANAGER_H */
