#pragma once

#include "plugin.h"

/* Структура профиля */
struct ProfileSettings {
    gchar *name;
    gchar *icon;
    gchar *default_name;
    guint32 enum_value;
};

/* Создание и освобождение профилей */
ProfileSettings* profile_settings_new(guint32 enum_value, const gchar *default_name);
void profile_settings_free(ProfileSettings *settings);

/* Работа с профилями */
void create_fallback_profiles(AsusdBatteryPlugin *plugin);
void parse_profile_choices(AsusdBatteryPlugin *plugin, GVariant *value);
gchar** asusd_get_available_profiles(AsusdBatteryPlugin *plugin);
const gchar* profile_name_from_enum(AsusdBatteryPlugin *plugin, guint32 enum_value);
gboolean profile_enum_from_name(AsusdBatteryPlugin *plugin, const gchar *name, guint32 *enum_value);
