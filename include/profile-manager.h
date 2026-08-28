/* include/profile-manager.h */
#ifndef __PROFILE_MANAGER_H__
#define __PROFILE_MANAGER_H__

#include <glib.h>
#include <gio/gio.h>
#include <xfconf/xfconf.h>

G_BEGIN_DECLS

typedef struct _AsusdBatteryPlugin AsusdBatteryPlugin;

/* Profile settings structure - FULL DEFINITION */
typedef struct _ProfileSettings {
    guint32 enum_value;
    gchar *default_name;
    gchar *name;
    gchar *icon;
} ProfileSettings;

/* Profile functions */
ProfileSettings* profile_settings_new(guint32 enum_value, const gchar *default_name);
void profile_settings_free(ProfileSettings *settings);

const gchar* profile_name_from_enum(AsusdBatteryPlugin *plugin, guint32 enum_value);
gboolean profile_enum_from_name(AsusdBatteryPlugin *plugin, const gchar *name, guint32 *enum_value);

void create_fallback_profiles(AsusdBatteryPlugin *plugin);
void parse_profile_choices(AsusdBatteryPlugin *plugin, GVariant *value);
gchar** asusd_get_available_profiles(AsusdBatteryPlugin *plugin);

G_END_DECLS

#endif /* __PROFILE_MANAGER_H__ */
