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
#define ASUSD_TIMEOUT_MS 5000

/* Состояния ASUSD */
typedef enum {
    ASUSD_STATE_UNAVAILABLE,
    ASUSD_STATE_CONNECTING,
    ASUSD_STATE_AVAILABLE
} AsusdState;

typedef struct ProfileSettings ProfileSettings;
typedef struct SettingsDialogState SettingsDialogState;
typedef struct AsyncCallContext AsyncCallContext;
typedef struct SettingsApplyContext SettingsApplyContext;

/* Структура плагина */
typedef struct {
    XfcePanelPlugin *plugin;
    GtkWidget *button;
    GtkWidget *box;
    GtkWidget *image;
    GtkWidget *label;
    
    gchar *current_profile;
    GPtrArray *profiles;
    GHashTable *profile_lookup;
    
    gboolean hide_icon;
    gboolean hide_text;
    gboolean hide_notifications;
    
    gboolean battery_limit_enabled;
    gboolean auto_switch_ac_enabled;
    gboolean auto_switch_battery_enabled;
    gchar *auto_switch_ac_profile;
    gchar *auto_switch_battery_profile;
    guint current_battery_limit;
    
    GDBusProxy *asusd_proxy;
    GDBusProxy *upower_proxy;
    GDBusConnection *connection;
    
    gboolean is_on_ac;
    
    AsusdState asusd_state;
    guint asusd_retry_timeout_id;
    guint asusd_init_retry_count;
    
    gchar *last_displayed_profile;
    time_t last_notification_time;
    
    gboolean settings_dialog_open;
    gboolean saving_settings;
    
    SettingsDialogState *dialog_state;
    
    GQueue *operation_queue;
    gboolean processing_ops;
    guint pending_calls;
    GCancellable *cancellable;
    gint init_load_state;
} AsusdBatteryPlugin;

/* Структура профиля */
struct ProfileSettings {
    gchar *name;
    gchar *icon;
    gchar *default_name;
    guint32 enum_value;
};

/* Структура состояния диалога настроек */
struct SettingsDialogState {
    GtkWidget *dialog;
    GtkWidget *check_ac;
    GtkWidget *check_battery;
    GtkWidget *combo_ac;
    GtkWidget *combo_battery;
    GtkWidget *limit_check;
    GtkWidget *hide_icon_check;
    GtkWidget *hide_text_check;
    GtkWidget *notifications_check;
    
    gboolean dirty_ac_enabled;
    gboolean dirty_battery_enabled;
    gboolean dirty_ac_profile;
    gboolean dirty_battery_profile;
    gboolean dirty_limit;
    
    gboolean syncing_ui;
};

/* Контекст для асинхронных операций */
struct AsyncCallContext {
    AsusdBatteryPlugin *plugin;
    gchar *method_name;
    GVariant *value;
    GAsyncReadyCallback callback;
    gpointer user_data;
    GDestroyNotify destroy_notify;
    gint ref_count;
};

/* Контекст для применения настроек */
struct SettingsApplyContext {
    AsusdBatteryPlugin *plugin;
    gboolean new_ac_enabled;
    gboolean new_battery_enabled;
    gchar *new_ac_profile;
    gchar *new_battery_profile;
    gboolean new_limit_enabled;
    guint8 new_limit;
    gint current_step;
    gint total_steps;
    gboolean has_errors;
    gchar **error_messages;
    gint error_count;
};

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
static void create_about_dialog(AsusdBatteryPlugin *plugin);
static void on_one_shot_clicked(GtkButton *button, GtkWidget *dialog);
static void settings_dialog_sync_from_asusd(AsusdBatteryPlugin *plugin, gboolean keep_dirty);
static void settings_dialog_reset_dirty(AsusdBatteryPlugin *plugin);
static void settings_dialog_update_ui(AsusdBatteryPlugin *plugin);

/* Прототипы для диалога настроек */
static void on_ac_switch_loaded_for_dialog(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_battery_switch_loaded_for_dialog(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_ac_profile_loaded_for_dialog(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_battery_profile_loaded_for_dialog(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_limit_loaded_for_dialog(GObject *source, GAsyncResult *res, gpointer user_data);

/* Прототипы для применения настроек */
static void apply_next_setting(SettingsApplyContext *ctx);
static void on_settings_apply_step_done(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_settings_apply_complete(SettingsApplyContext *ctx);

/* ASUSD прототипы */
static void asusd_init_async(AsusdBatteryPlugin *plugin);
static void asusd_cleanup(AsusdBatteryPlugin *plugin);
static void asusd_set_profile_async(AsusdBatteryPlugin *plugin, const gchar *profile_name,
                                    GAsyncReadyCallback callback, gpointer user_data);
static gboolean asusd_retry_init(gpointer user_data);
static void create_asusd_proxy_async(AsusdBatteryPlugin *plugin);
static void create_upower_proxy_async(AsusdBatteryPlugin *plugin);
static void on_proxy_properties_changed(GDBusProxy *proxy, GVariant *changed_properties,
                                        GStrv invalidated_properties, gpointer user_data);
static void asusd_get_property_async(AsusdBatteryPlugin *plugin, const char *property,
                                     GAsyncReadyCallback callback, gpointer user_data);
static void asusd_set_property_async(AsusdBatteryPlugin *plugin, const char *property,
                                     GVariant *value, GAsyncReadyCallback callback,
                                     gpointer user_data);
static void asusd_call_async(AsusdBatteryPlugin *plugin, const char *method,
                             GVariant *parameters, GAsyncReadyCallback callback,
                             gpointer user_data);
static void asusd_queue_operation(AsusdBatteryPlugin *plugin, const char *method,
                                  GVariant *parameters, GAsyncReadyCallback callback,
                                  gpointer user_data);
static void process_next_operation(AsusdBatteryPlugin *plugin);
static ProfileSettings* profile_settings_new(guint32 enum_value, const gchar *default_name);
static void profile_settings_free(ProfileSettings *settings);
static const gchar* get_profile_icon(AsusdBatteryPlugin *plugin, const gchar *profile);
static gboolean can_send_notification(AsusdBatteryPlugin *plugin);
static const gchar* profile_name_from_enum(AsusdBatteryPlugin *plugin, guint32 enum_value);
static gboolean profile_enum_from_name(AsusdBatteryPlugin *plugin, const gchar *name, guint32 *enum_value);
static gchar** asusd_get_available_profiles(AsusdBatteryPlugin *plugin);
static void create_fallback_profiles(AsusdBatteryPlugin *plugin);
static void parse_profile_choices(AsusdBatteryPlugin *plugin, GVariant *value);

/* Асинхронные callback прототипы */
static void on_asusd_proxy_created(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_upower_proxy_created(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_profile_choices_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_current_profile_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_limit_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_ac_switch_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_battery_switch_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_ac_profile_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_battery_profile_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_set_profile_done(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_one_shot_done(GObject *source, GAsyncResult *res, gpointer user_data);
static void on_property_set_done(GObject *source, GAsyncResult *res, gpointer user_data);

/* ========== Вспомогательные функции ========== */

static const gchar* get_profile_icon(AsusdBatteryPlugin *plugin, const gchar *profile) {
    if (!plugin || !profile) return "battery-good-symbolic";
    
    if (plugin->profile_lookup) {
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
    }
    
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
    if (current_time - plugin->last_notification_time < 2) return FALSE;
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
    if (!plugin) return "balanced";
    
    if (!plugin->profile_lookup) {
        g_debug("profile_name_from_enum: profile_lookup is NULL, using fallback");
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

static gboolean profile_enum_from_name(AsusdBatteryPlugin *plugin, const gchar *name, guint32 *enum_value) {
    if (!plugin || !name || !enum_value) return FALSE;
    
    if (!plugin->profile_lookup) {
        g_debug("profile_enum_from_name: profile_lookup is NULL");
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

static gchar** asusd_get_available_profiles(AsusdBatteryPlugin *plugin) {
    if (!plugin) return NULL;
    
    if (!plugin->profiles || plugin->profiles->len == 0) {
        g_debug("asusd_get_available_profiles: no profiles loaded, creating fallback");
        create_fallback_profiles(plugin);
    }
    
    if (!plugin->profiles || plugin->profiles->len == 0) {
        g_debug("asusd_get_available_profiles: still no profiles, returning empty");
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

static void create_fallback_profiles(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    
    /* Проверяем, есть ли уже профили */
    if (plugin->profiles && plugin->profiles->len > 0) {
        g_debug("ASUSD: Profiles already exist (%d), not creating fallback", plugin->profiles->len);
        return;
    }
    
    g_debug("ASUSD: Creating fallback profiles");
    
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
        g_debug("ASUSD: Added fallback profile: %s (enum: %d)", default_profiles[i], i);
    }
}

static void parse_profile_choices(AsusdBatteryPlugin *plugin, GVariant *value) {
    if (!plugin || !value) return;
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
        g_debug("ASUSD: Loaded profile: %s (enum: %u)", default_name, enum_value);
    }
}

XFCE_PANEL_PLUGIN_REGISTER(asusd_battery_plugin_construct)

/* ========== Асинхронные операции ========== */

static AsyncCallContext* async_call_context_new(AsusdBatteryPlugin *plugin,
                                                const char *method_name,
                                                GVariant *value,
                                                GAsyncReadyCallback callback,
                                                gpointer user_data,
                                                GDestroyNotify destroy_notify) {
    AsyncCallContext *ctx = g_new0(AsyncCallContext, 1);
    ctx->plugin = plugin;
    ctx->method_name = g_strdup(method_name);
    if (value) ctx->value = g_variant_ref(value);
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->destroy_notify = destroy_notify;
    ctx->ref_count = 1;
    return ctx;
}

static void async_call_context_free(AsyncCallContext *ctx) {
    if (!ctx) return;
    if (g_atomic_int_dec_and_test(&ctx->ref_count)) {
        g_free(ctx->method_name);
        if (ctx->value) g_variant_unref(ctx->value);
        if (ctx->destroy_notify && ctx->user_data) ctx->destroy_notify(ctx->user_data);
        g_free(ctx);
    }
}

static void asusd_queue_operation(AsusdBatteryPlugin *plugin, const char *method,
                                  GVariant *parameters, GAsyncReadyCallback callback,
                                  gpointer user_data) {
    if (!plugin || !method) return;
    AsyncCallContext *ctx = async_call_context_new(plugin, method, parameters, callback, user_data, NULL);
    g_queue_push_tail(plugin->operation_queue, ctx);
    if (!plugin->processing_ops) process_next_operation(plugin);
}

static void process_next_operation(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    if (g_queue_is_empty(plugin->operation_queue)) { plugin->processing_ops = FALSE; return; }
    if (!plugin->asusd_proxy) { g_debug("ASUSD: Proxy not available"); plugin->processing_ops = FALSE; return; }
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) {
        g_debug("ASUSD: Not available (state=%d)", plugin->asusd_state);
        plugin->processing_ops = FALSE;
        if (plugin->asusd_retry_timeout_id == 0)
            plugin->asusd_retry_timeout_id = g_timeout_add_seconds(2, asusd_retry_init, plugin);
        return;
    }
    
    AsyncCallContext *ctx = g_queue_pop_head(plugin->operation_queue);
    if (!ctx) { plugin->processing_ops = FALSE; return; }
    
    plugin->processing_ops = TRUE;
    plugin->pending_calls++;
    
    g_debug("ASUSD: Processing operation: %s", ctx->method_name);
    if (ctx->value) {
        gchar *params_str = g_variant_print(ctx->value, TRUE);
        g_debug("ASUSD: Params: %s", params_str);
        g_free(params_str);
    }
    
    g_dbus_proxy_call(
        plugin->asusd_proxy,
        ctx->method_name,
        ctx->value,
        G_DBUS_CALL_FLAGS_NONE,
        ASUSD_TIMEOUT_MS,
        plugin->cancellable,
        (GAsyncReadyCallback)on_property_set_done,
        ctx
    );
}

static void on_property_set_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsyncCallContext *ctx = (AsyncCallContext*)user_data;
    AsusdBatteryPlugin *plugin = ctx->plugin;
    plugin->pending_calls--;
    
    GError *error = NULL;
    GDBusProxy *proxy = G_DBUS_PROXY(source);
    GVariant *result = g_dbus_proxy_call_finish(proxy, res, &error);
    
    if (error) {
        g_warning("ASUSD: Operation failed: %s", error->message);
        if (ctx->callback) {
            GAsyncReadyCallback callback = ctx->callback;
            callback(source, res, ctx->user_data);
        }
        g_error_free(error);
    } else {
        if (ctx->callback) {
            GAsyncReadyCallback callback = ctx->callback;
            callback(source, res, ctx->user_data);
        }
        if (result) g_variant_unref(result);
    }
    
    async_call_context_free(ctx);
    process_next_operation(plugin);
}

static void asusd_call_async(AsusdBatteryPlugin *plugin, const char *method,
                             GVariant *parameters, GAsyncReadyCallback callback,
                             gpointer user_data) {
    if (!plugin || !method) return;
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) {
        g_debug("ASUSD: Call %s queued (state=%d)", method, plugin->asusd_state);
    }
    asusd_queue_operation(plugin, method, parameters, callback, user_data);
}

/* ========== Работа со свойствами через GDBusConnection ========== */

static void asusd_get_property_async(AsusdBatteryPlugin *plugin, const char *property,
                                     GAsyncReadyCallback callback, gpointer user_data) {
    if (!plugin || !property) return;
    g_debug("ASUSD: Getting property: %s", property);
    
    if (!plugin->connection) {
        g_warning("ASUSD: No connection available");
        return;
    }
    
    GVariant *params = g_variant_new("(ss)", ASUSD_INTERFACE, property);
    g_dbus_connection_call(
        plugin->connection,
        ASUSD_BUS_NAME,
        ASUSD_OBJECT_PATH,
        DBUS_PROPERTIES_INTERFACE,
        "Get",
        params,
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        ASUSD_TIMEOUT_MS,
        plugin->cancellable,
        callback,
        user_data
    );
}

static void asusd_set_property_async(AsusdBatteryPlugin *plugin, const char *property,
                                     GVariant *value, GAsyncReadyCallback callback,
                                     gpointer user_data) {
    if (!plugin || !property || !value) return;
    if (!plugin->connection) {
        g_warning("ASUSD: No connection available");
        return;
    }
    
    GVariant *params = g_variant_new("(ssv)", ASUSD_INTERFACE, property, value);
    g_dbus_connection_call(
        plugin->connection,
        ASUSD_BUS_NAME,
        ASUSD_OBJECT_PATH,
        DBUS_PROPERTIES_INTERFACE,
        "Set",
        params,
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        ASUSD_TIMEOUT_MS,
        plugin->cancellable,
        callback,
        user_data
    );
}

static void asusd_set_profile_async(AsusdBatteryPlugin *plugin, const gchar *profile_name,
                                    GAsyncReadyCallback callback, gpointer user_data) {
    if (!plugin || !profile_name) return;
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) return;
    guint32 enum_val = 999;
    if (!profile_enum_from_name(plugin, profile_name, &enum_val)) {
        g_warning("ASUSD: Profile %s not found", profile_name);
        return;
    }
    asusd_set_property_async(plugin, "PlatformProfile", g_variant_new_uint32(enum_val), callback, user_data);
}

/* ========== Создание прокси ========== */

static void create_asusd_proxy_async(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    g_debug("ASUSD: Creating proxy asynchronously");
    
    g_dbus_proxy_new_for_bus(
        G_BUS_TYPE_SYSTEM,
        G_DBUS_PROXY_FLAGS_NONE,
        NULL,
        ASUSD_BUS_NAME,
        ASUSD_OBJECT_PATH,
        ASUSD_INTERFACE,
        plugin->cancellable,
        (GAsyncReadyCallback)on_asusd_proxy_created,
        plugin
    );
}

static void on_asusd_proxy_created(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin*)user_data;
    GError *error = NULL;
    
    plugin->asusd_proxy = g_dbus_proxy_new_for_bus_finish(res, &error);
    if (error) {
        g_warning("ASUSD: Failed to create proxy: %s", error->message);
        g_error_free(error);
        plugin->asusd_state = ASUSD_STATE_UNAVAILABLE;
        if (plugin->asusd_retry_timeout_id == 0)
            plugin->asusd_retry_timeout_id = g_timeout_add_seconds(5, asusd_retry_init, plugin);
        return;
    }
    if (!plugin->asusd_proxy) {
        g_warning("ASUSD: Proxy creation returned NULL");
        plugin->asusd_state = ASUSD_STATE_UNAVAILABLE;
        return;
    }
    
    g_debug("ASUSD: Proxy created successfully");
    plugin->connection = g_dbus_proxy_get_connection(plugin->asusd_proxy);
    if (plugin->connection) g_object_ref(plugin->connection);
    
    g_signal_connect(plugin->asusd_proxy, "g-properties-changed",
                    G_CALLBACK(on_proxy_properties_changed), plugin);
    
    gchar *owner = g_dbus_proxy_get_name_owner(plugin->asusd_proxy);
    g_debug("ASUSD: Current name owner = %s", owner ? owner : "NULL");
    
    if (owner) {
        g_free(owner);
        g_debug("ASUSD: Owner exists, loading initial data...");
        plugin->asusd_state = ASUSD_STATE_AVAILABLE;
        plugin->init_load_state = 0;
        asusd_get_property_async(plugin, "PlatformProfileChoices",
                                (GAsyncReadyCallback)on_profile_choices_loaded, plugin);
    } else {
        g_free(owner);
        g_debug("ASUSD: No owner yet, will retry");
        plugin->asusd_state = ASUSD_STATE_UNAVAILABLE;
        /* Создаем fallback профили, чтобы плагин работал */
        create_fallback_profiles(plugin);
        if (plugin->asusd_retry_timeout_id == 0)
            plugin->asusd_retry_timeout_id = g_timeout_add_seconds(2, asusd_retry_init, plugin);
    }
}

static void create_upower_proxy_async(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    g_debug("UPower: Creating proxy asynchronously");
    g_dbus_proxy_new_for_bus(
        G_BUS_TYPE_SYSTEM,
        G_DBUS_PROXY_FLAGS_NONE,
        NULL,
        "org.freedesktop.UPower",
        "/org/freedesktop/UPower",
        "org.freedesktop.UPower",
        plugin->cancellable,
        (GAsyncReadyCallback)on_upower_proxy_created,
        plugin
    );
}

static void on_upower_proxy_created(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin*)user_data;
    GError *error = NULL;
    plugin->upower_proxy = g_dbus_proxy_new_for_bus_finish(res, &error);
    if (error) {
        g_warning("UPower: Failed to create proxy: %s", error->message);
        g_error_free(error);
        return;
    }
    g_debug("UPower: Proxy created successfully");
    g_signal_connect(plugin->upower_proxy, "g-properties-changed",
                    G_CALLBACK(on_proxy_properties_changed), plugin);
}

/* ========== Сигналы прокси ========== */

static void on_proxy_properties_changed(GDBusProxy *proxy,
                                        GVariant *changed_properties,
                                        GStrv invalidated_properties,
                                        gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin *)user_data;
    if (!plugin || !changed_properties) return;
    g_debug("=== on_proxy_properties_changed ===");
    
    if (proxy == plugin->asusd_proxy) {
        g_debug("  From ASUSD proxy");
        GVariantIter iter;
        gchar *key;
        GVariant *value;
        g_variant_iter_init(&iter, changed_properties);
        while (g_variant_iter_next(&iter, "{sv}", &key, &value)) {
            g_debug("  changed property: %s", key);
            if (g_strcmp0(key, "PlatformProfile") == 0) {
                guint32 enum_val;
                g_variant_get(value, "u", &enum_val);
                const gchar *name = profile_name_from_enum(plugin, enum_val);
                if (!plugin->current_profile || g_strcmp0(plugin->current_profile, name) != 0) {
                    g_free(plugin->current_profile);
                    plugin->current_profile = g_strdup(name);
                    update_profile_display(plugin, TRUE);
                }
            } else if (g_strcmp0(key, "ChargeControlEndThreshold") == 0) {
                guint8 limit;
                g_variant_get(value, "y", &limit);
                plugin->current_battery_limit = limit;
                plugin->battery_limit_enabled = (limit == 80);
            } else if (g_strcmp0(key, "ChangePlatformProfileOnAc") == 0) {
                g_variant_get(value, "b", &plugin->auto_switch_ac_enabled);
                if (plugin->dialog_state && plugin->dialog_state->check_ac && !plugin->dialog_state->dirty_ac_enabled) {
                    plugin->dialog_state->syncing_ui = TRUE;
                    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->check_ac), plugin->auto_switch_ac_enabled);
                    plugin->dialog_state->syncing_ui = FALSE;
                }
            } else if (g_strcmp0(key, "ChangePlatformProfileOnBattery") == 0) {
                g_variant_get(value, "b", &plugin->auto_switch_battery_enabled);
                if (plugin->dialog_state && plugin->dialog_state->check_battery && !plugin->dialog_state->dirty_battery_enabled) {
                    plugin->dialog_state->syncing_ui = TRUE;
                    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->check_battery), plugin->auto_switch_battery_enabled);
                    plugin->dialog_state->syncing_ui = FALSE;
                }
            } else if (g_strcmp0(key, "PlatformProfileOnAc") == 0) {
                guint32 enum_val;
                g_variant_get(value, "u", &enum_val);
                const gchar *name = profile_name_from_enum(plugin, enum_val);
                g_free(plugin->auto_switch_ac_profile);
                plugin->auto_switch_ac_profile = g_strdup(name);
                if (plugin->dialog_state && plugin->dialog_state->combo_ac && !plugin->dialog_state->dirty_ac_profile) {
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
                        if (found) {
                            plugin->dialog_state->syncing_ui = TRUE;
                            gtk_combo_box_set_active(GTK_COMBO_BOX(plugin->dialog_state->combo_ac), index);
                            plugin->dialog_state->syncing_ui = FALSE;
                        }
                    }
                }
            } else if (g_strcmp0(key, "PlatformProfileOnBattery") == 0) {
                guint32 enum_val;
                g_variant_get(value, "u", &enum_val);
                const gchar *name = profile_name_from_enum(plugin, enum_val);
                g_free(plugin->auto_switch_battery_profile);
                plugin->auto_switch_battery_profile = g_strdup(name);
                if (plugin->dialog_state && plugin->dialog_state->combo_battery && !plugin->dialog_state->dirty_battery_profile) {
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
                        if (found) {
                            plugin->dialog_state->syncing_ui = TRUE;
                            gtk_combo_box_set_active(GTK_COMBO_BOX(plugin->dialog_state->combo_battery), index);
                            plugin->dialog_state->syncing_ui = FALSE;
                        }
                    }
                }
            }
            g_free(key);
            g_variant_unref(value);
        }
    } else if (proxy == plugin->upower_proxy) {
        g_debug("  From UPower proxy");
        GVariantDict dict;
        g_variant_dict_init(&dict, changed_properties);
        GVariant *value = g_variant_dict_lookup_value(&dict, "OnBattery", NULL);
        if (value) {
            gboolean on_battery = g_variant_get_boolean(value);
            plugin->is_on_ac = !on_battery;
            g_debug("  OnBattery = %d, is_on_ac = %d", on_battery, plugin->is_on_ac);
            g_variant_unref(value);
        }
        g_variant_dict_clear(&dict);
    }
}

/* ========== Асинхронная инициализация ========== */

static void asusd_init_async(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    g_debug("ASUSD: Initializing asynchronously...");
    if (plugin->asusd_proxy) {
        g_debug("ASUSD: Cleaning up old proxy");
        g_signal_handlers_disconnect_by_data(plugin->asusd_proxy, plugin);
        g_object_unref(plugin->asusd_proxy);
        plugin->asusd_proxy = NULL;
        if (plugin->connection) { g_object_unref(plugin->connection); plugin->connection = NULL; }
    }
    if (plugin->profiles) { g_ptr_array_free(plugin->profiles, TRUE); plugin->profiles = NULL; }
    if (plugin->profile_lookup) { g_hash_table_destroy(plugin->profile_lookup); plugin->profile_lookup = NULL; }
    plugin->asusd_state = ASUSD_STATE_CONNECTING;
    create_asusd_proxy_async(plugin);
}

static gboolean asusd_retry_init(gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin *)user_data;
    if (!plugin) return G_SOURCE_REMOVE;
    plugin->asusd_retry_timeout_id = 0;
    g_debug("ASUSD: Retry init attempt %d", plugin->asusd_init_retry_count + 1);
    plugin->asusd_init_retry_count++;
    if (plugin->asusd_proxy) {
        gchar *owner = g_dbus_proxy_get_name_owner(plugin->asusd_proxy);
        if (owner) {
            g_debug("ASUSD: Owner found on retry: %s", owner);
            g_free(owner);
            plugin->asusd_state = ASUSD_STATE_AVAILABLE;
            asusd_get_property_async(plugin, "PlatformProfileChoices",
                                    (GAsyncReadyCallback)on_profile_choices_loaded, plugin);
            return G_SOURCE_REMOVE;
        }
        g_free(owner);
    }
    asusd_init_async(plugin);
    return G_SOURCE_REMOVE;
}

static void on_profile_choices_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin*)user_data;
    GError *error = NULL;
    
    /* Если профили уже есть, не создаем их заново */
    if (plugin->profiles && plugin->profiles->len > 0) {
        g_debug("ASUSD: Profiles already loaded, skipping");
        return;
    }
    
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error) {
        if (g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD)) {
            g_warning("ASUSD: PlatformProfileChoices not supported, using fallback");
            g_error_free(error);
            create_fallback_profiles(plugin);
            asusd_get_property_async(plugin, "PlatformProfile", (GAsyncReadyCallback)on_current_profile_loaded, plugin);
            return;
        }
        g_warning("ASUSD: Failed to get PlatformProfileChoices: %s", error->message);
        g_error_free(error);
        plugin->asusd_state = ASUSD_STATE_UNAVAILABLE;
        if (plugin->asusd_retry_timeout_id == 0)
            plugin->asusd_retry_timeout_id = g_timeout_add_seconds(5, asusd_retry_init, plugin);
        return;
    }
    if (!result) {
        g_warning("ASUSD: No result for PlatformProfileChoices");
        create_fallback_profiles(plugin);
        asusd_get_property_async(plugin, "PlatformProfile", (GAsyncReadyCallback)on_current_profile_loaded, plugin);
        return;
    }
    
    GVariant *value = NULL;
    g_variant_get(result, "(v)", &value);
    g_variant_unref(result);
    if (!value) {
        g_warning("ASUSD: No value in PlatformProfileChoices");
        create_fallback_profiles(plugin);
        asusd_get_property_async(plugin, "PlatformProfile", (GAsyncReadyCallback)on_current_profile_loaded, plugin);
        return;
    }
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
        GVariant *inner = g_variant_get_variant(value);
        g_variant_unref(value);
        value = inner;
    }
    if (!g_variant_is_of_type(value, G_VARIANT_TYPE_ARRAY)) {
        g_warning("ASUSD: PlatformProfileChoices not array");
        g_variant_unref(value);
        create_fallback_profiles(plugin);
        asusd_get_property_async(plugin, "PlatformProfile", (GAsyncReadyCallback)on_current_profile_loaded, plugin);
        return;
    }
    parse_profile_choices(plugin, value);
    g_variant_unref(value);
    asusd_get_property_async(plugin, "PlatformProfile", (GAsyncReadyCallback)on_current_profile_loaded, plugin);
}

static void on_current_profile_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin*)user_data;
    GError *error = NULL;
    
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error) {
        g_debug("ASUSD: PlatformProfile query failed: %s", error->message);
        g_error_free(error);
        if (!plugin->current_profile || g_strcmp0(plugin->current_profile, "unknown") == 0) {
            g_free(plugin->current_profile);
            plugin->current_profile = g_strdup("balanced");
            plugin->asusd_state = ASUSD_STATE_AVAILABLE;
            update_profile_display(plugin, FALSE);
        }
        return;
    }
    if (!result) {
        g_debug("ASUSD: No result for PlatformProfile");
        if (!plugin->current_profile || g_strcmp0(plugin->current_profile, "unknown") == 0) {
            g_free(plugin->current_profile);
            plugin->current_profile = g_strdup("balanced");
            plugin->asusd_state = ASUSD_STATE_AVAILABLE;
            update_profile_display(plugin, FALSE);
        }
        return;
    }
    
    GVariant *value = NULL;
    g_variant_get(result, "(v)", &value);
    g_variant_unref(result);
    if (!value) {
        g_debug("ASUSD: No value in PlatformProfile");
        if (!plugin->current_profile || g_strcmp0(plugin->current_profile, "unknown") == 0) {
            g_free(plugin->current_profile);
            plugin->current_profile = g_strdup("balanced");
            plugin->asusd_state = ASUSD_STATE_AVAILABLE;
            update_profile_display(plugin, FALSE);
        }
        return;
    }
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
        GVariant *inner = g_variant_get_variant(value);
        g_variant_unref(value);
        value = inner;
    }
    if (!g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32)) {
        g_warning("ASUSD: PlatformProfile not uint32");
        g_variant_unref(value);
        if (!plugin->current_profile || g_strcmp0(plugin->current_profile, "unknown") == 0) {
            g_free(plugin->current_profile);
            plugin->current_profile = g_strdup("balanced");
            plugin->asusd_state = ASUSD_STATE_AVAILABLE;
            update_profile_display(plugin, FALSE);
        }
        return;
    }
    guint32 enum_val;
    g_variant_get(value, "u", &enum_val);
    g_variant_unref(value);
    const gchar *name = profile_name_from_enum(plugin, enum_val);
    g_free(plugin->current_profile);
    plugin->current_profile = g_strdup(name);
    plugin->asusd_state = ASUSD_STATE_AVAILABLE;
    update_profile_display(plugin, FALSE);
    
    if (plugin->init_load_state == 0) {
        plugin->init_load_state = 1;
        asusd_get_property_async(plugin, "ChargeControlEndThreshold", (GAsyncReadyCallback)on_limit_loaded, plugin);
    }
}

static void on_limit_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin*)user_data;
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error) {
        g_debug("ASUSD: Failed to get ChargeControlEndThreshold: %s", error->message);
        g_error_free(error);
    } else if (result) {
        GVariant *value = NULL;
        g_variant_get(result, "(v)", &value);
        g_variant_unref(result);
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
                g_debug("ASUSD: ChargeControlEndThreshold = %d", limit);
            }
            g_variant_unref(value);
        }
    }
    asusd_get_property_async(plugin, "ChangePlatformProfileOnAc", (GAsyncReadyCallback)on_ac_switch_loaded, plugin);
}

static void on_ac_switch_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin*)user_data;
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error) {
        g_debug("ASUSD: Failed to get ChangePlatformProfileOnAc: %s", error->message);
        g_error_free(error);
    } else if (result) {
        GVariant *value = NULL;
        g_variant_get(result, "(v)", &value);
        g_variant_unref(result);
        if (value) {
            if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
                GVariant *inner = g_variant_get_variant(value);
                g_variant_unref(value);
                value = inner;
            }
            if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
                g_variant_get(value, "b", &plugin->auto_switch_ac_enabled);
                g_debug("ASUSD: ChangePlatformProfileOnAc = %d", plugin->auto_switch_ac_enabled);
            }
            g_variant_unref(value);
        }
    }
    asusd_get_property_async(plugin, "PlatformProfileOnAc", (GAsyncReadyCallback)on_ac_profile_loaded, plugin);
}

static void on_ac_profile_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin*)user_data;
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error) {
        g_debug("ASUSD: Failed to get PlatformProfileOnAc: %s", error->message);
        g_error_free(error);
    } else if (result) {
        GVariant *value = NULL;
        g_variant_get(result, "(v)", &value);
        g_variant_unref(result);
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
                g_debug("ASUSD: PlatformProfileOnAc = %s", name);
            }
            g_variant_unref(value);
        }
    }
    asusd_get_property_async(plugin, "ChangePlatformProfileOnBattery", (GAsyncReadyCallback)on_battery_switch_loaded, plugin);
}

static void on_battery_switch_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin*)user_data;
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error) {
        g_debug("ASUSD: Failed to get ChangePlatformProfileOnBattery: %s", error->message);
        g_error_free(error);
    } else if (result) {
        GVariant *value = NULL;
        g_variant_get(result, "(v)", &value);
        g_variant_unref(result);
        if (value) {
            if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
                GVariant *inner = g_variant_get_variant(value);
                g_variant_unref(value);
                value = inner;
            }
            if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
                g_variant_get(value, "b", &plugin->auto_switch_battery_enabled);
                g_debug("ASUSD: ChangePlatformProfileOnBattery = %d", plugin->auto_switch_battery_enabled);
            }
            g_variant_unref(value);
        }
    }
    asusd_get_property_async(plugin, "PlatformProfileOnBattery", (GAsyncReadyCallback)on_battery_profile_loaded, plugin);
}

static void on_battery_profile_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin*)user_data;
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error) {
        g_debug("ASUSD: Failed to get PlatformProfileOnBattery: %s", error->message);
        g_error_free(error);
    } else if (result) {
        GVariant *value = NULL;
        g_variant_get(result, "(v)", &value);
        g_variant_unref(result);
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
                g_debug("ASUSD: PlatformProfileOnBattery = %s", name);
            }
            g_variant_unref(value);
        }
    }
    
    /* Если профили еще не загружены, создаем fallback */
    if (!plugin->profiles || plugin->profiles->len == 0) {
        g_debug("ASUSD: No profiles loaded after init, creating fallback");
        create_fallback_profiles(plugin);
    }
    
    plugin->asusd_state = ASUSD_STATE_AVAILABLE;
    plugin->asusd_init_retry_count = 0;
    update_profile_display(plugin, FALSE);
    g_debug("ASUSD: Async initialization completed");
    if (plugin->dialog_state && plugin->dialog_state->dialog)
        settings_dialog_sync_from_asusd(plugin, FALSE);
}

/* Очистка ASUSD */
static void asusd_cleanup(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    if (plugin->cancellable) { g_cancellable_cancel(plugin->cancellable); g_object_unref(plugin->cancellable); plugin->cancellable = NULL; }
    if (plugin->connection) { g_object_unref(plugin->connection); plugin->connection = NULL; }
    if (plugin->asusd_proxy) { g_signal_handlers_disconnect_by_data(plugin->asusd_proxy, plugin); g_object_unref(plugin->asusd_proxy); plugin->asusd_proxy = NULL; }
    if (plugin->upower_proxy) { g_signal_handlers_disconnect_by_data(plugin->upower_proxy, plugin); g_object_unref(plugin->upower_proxy); plugin->upower_proxy = NULL; }
    if (plugin->profiles) { g_ptr_array_free(plugin->profiles, TRUE); plugin->profiles = NULL; }
    if (plugin->profile_lookup) { g_hash_table_destroy(plugin->profile_lookup); plugin->profile_lookup = NULL; }
    if (plugin->operation_queue) { g_queue_free_full(plugin->operation_queue, (GDestroyNotify)async_call_context_free); plugin->operation_queue = NULL; }
    if (plugin->asusd_retry_timeout_id > 0) { g_source_remove(plugin->asusd_retry_timeout_id); plugin->asusd_retry_timeout_id = 0; }
    plugin->asusd_state = ASUSD_STATE_UNAVAILABLE;
    plugin->processing_ops = FALSE;
    plugin->pending_calls = 0;
}

/* ========== Функции диалога настроек ========== */

static void settings_dialog_reset_dirty(AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->dialog_state) return;
    plugin->dialog_state->dirty_ac_enabled = FALSE;
    plugin->dialog_state->dirty_battery_enabled = FALSE;
    plugin->dialog_state->dirty_ac_profile = FALSE;
    plugin->dialog_state->dirty_battery_profile = FALSE;
    plugin->dialog_state->dirty_limit = FALSE;
}

static void settings_dialog_update_ui(AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->dialog_state) return;
    SettingsDialogState *state = plugin->dialog_state;
    state->syncing_ui = TRUE;
    
    /* Проверяем, загружены ли профили */
    if (!plugin->profiles || plugin->profiles->len == 0) {
        g_debug("settings_dialog_update_ui: No profiles loaded, creating fallback");
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

static void settings_dialog_sync_from_asusd(AsusdBatteryPlugin *plugin, gboolean keep_dirty) {
    if (!plugin || !plugin->dialog_state) return;
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) {
        g_debug("settings_dialog_sync_from_asusd: ASUSD not available, using cached values");
        settings_dialog_update_ui(plugin);
        return;
    }
    plugin->dialog_state->syncing_ui = TRUE;
    g_debug("settings_dialog_sync_from_asusd: Loading settings from ASUSD");
    asusd_get_property_async(plugin, "ChangePlatformProfileOnAc",
                            (GAsyncReadyCallback)on_ac_switch_loaded_for_dialog, plugin);
}

static void on_ac_switch_loaded_for_dialog(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin*)user_data;
    GError *error = NULL;
    
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error || !result) {
        if (error) g_debug("Failed to get ChangePlatformProfileOnAc: %s", error->message);
        else g_debug("No result for ChangePlatformProfileOnAc");
        if (error) g_error_free(error);
        plugin->auto_switch_ac_enabled = FALSE;
        if (plugin->dialog_state && plugin->dialog_state->check_ac && !plugin->dialog_state->dirty_ac_enabled)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->check_ac), FALSE);
        asusd_get_property_async(plugin, "ChangePlatformProfileOnBattery",
                                (GAsyncReadyCallback)on_battery_switch_loaded_for_dialog, plugin);
        return;
    }
    
    GVariant *value = NULL;
    g_variant_get(result, "(v)", &value);
    g_variant_unref(result);
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
            g_debug("ChangePlatformProfileOnAc = %d", ac_enabled);
            if (plugin->dialog_state && plugin->dialog_state->check_ac && !plugin->dialog_state->dirty_ac_enabled)
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->check_ac), ac_enabled);
        }
        g_variant_unref(value);
    }
    asusd_get_property_async(plugin, "ChangePlatformProfileOnBattery",
                            (GAsyncReadyCallback)on_battery_switch_loaded_for_dialog, plugin);
}

static void on_battery_switch_loaded_for_dialog(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin*)user_data;
    GError *error = NULL;
    
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error || !result) {
        if (error) g_debug("Failed to get ChangePlatformProfileOnBattery: %s", error->message);
        else g_debug("No result for ChangePlatformProfileOnBattery");
        if (error) g_error_free(error);
        plugin->auto_switch_battery_enabled = FALSE;
        if (plugin->dialog_state && plugin->dialog_state->check_battery && !plugin->dialog_state->dirty_battery_enabled)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->check_battery), FALSE);
        asusd_get_property_async(plugin, "PlatformProfileOnAc",
                                (GAsyncReadyCallback)on_ac_profile_loaded_for_dialog, plugin);
        return;
    }
    
    GVariant *value = NULL;
    g_variant_get(result, "(v)", &value);
    g_variant_unref(result);
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
            g_debug("ChangePlatformProfileOnBattery = %d", battery_enabled);
            if (plugin->dialog_state && plugin->dialog_state->check_battery && !plugin->dialog_state->dirty_battery_enabled)
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->check_battery), battery_enabled);
        }
        g_variant_unref(value);
    }
    asusd_get_property_async(plugin, "PlatformProfileOnAc",
                            (GAsyncReadyCallback)on_ac_profile_loaded_for_dialog, plugin);
}

static void on_ac_profile_loaded_for_dialog(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin*)user_data;
    GError *error = NULL;
    
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error || !result) {
        if (error) g_debug("Failed to get PlatformProfileOnAc: %s", error->message);
        else g_debug("No result for PlatformProfileOnAc");
        if (error) g_error_free(error);
        const char *default_profile = "balanced";
        if (plugin->profiles && plugin->profiles->len > 0) {
            ProfileSettings *settings = g_ptr_array_index(plugin->profiles, 0);
            if (settings->default_name) default_profile = settings->default_name;
        }
        g_free(plugin->auto_switch_ac_profile);
        plugin->auto_switch_ac_profile = g_strdup(default_profile);
        asusd_get_property_async(plugin, "PlatformProfileOnBattery",
                                (GAsyncReadyCallback)on_battery_profile_loaded_for_dialog, plugin);
        return;
    }
    
    GVariant *value = NULL;
    g_variant_get(result, "(v)", &value);
    g_variant_unref(result);
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
            g_debug("PlatformProfileOnAc = %s", name);
            if (plugin->dialog_state && plugin->dialog_state->combo_ac && !plugin->dialog_state->dirty_ac_profile) {
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
    asusd_get_property_async(plugin, "PlatformProfileOnBattery",
                            (GAsyncReadyCallback)on_battery_profile_loaded_for_dialog, plugin);
}

static void on_battery_profile_loaded_for_dialog(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin*)user_data;
    GError *error = NULL;
    
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error || !result) {
        if (error) g_debug("Failed to get PlatformProfileOnBattery: %s", error->message);
        else g_debug("No result for PlatformProfileOnBattery");
        if (error) g_error_free(error);
        const char *default_profile = "balanced";
        if (plugin->profiles && plugin->profiles->len > 0) {
            ProfileSettings *settings = g_ptr_array_index(plugin->profiles, 0);
            if (settings->default_name) default_profile = settings->default_name;
        }
        g_free(plugin->auto_switch_battery_profile);
        plugin->auto_switch_battery_profile = g_strdup(default_profile);
        asusd_get_property_async(plugin, "ChargeControlEndThreshold",
                                (GAsyncReadyCallback)on_limit_loaded_for_dialog, plugin);
        return;
    }
    
    GVariant *value = NULL;
    g_variant_get(result, "(v)", &value);
    g_variant_unref(result);
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
            g_debug("PlatformProfileOnBattery = %s", name);
            if (plugin->dialog_state && plugin->dialog_state->combo_battery && !plugin->dialog_state->dirty_battery_profile) {
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
    asusd_get_property_async(plugin, "ChargeControlEndThreshold",
                            (GAsyncReadyCallback)on_limit_loaded_for_dialog, plugin);
}

static void on_limit_loaded_for_dialog(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin*)user_data;
    GError *error = NULL;
    
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error || !result) {
        if (error) g_debug("Failed to get ChargeControlEndThreshold: %s", error->message);
        else g_debug("No result for ChargeControlEndThreshold");
        if (error) g_error_free(error);
        plugin->current_battery_limit = 100;
        plugin->battery_limit_enabled = FALSE;
        if (plugin->dialog_state && plugin->dialog_state->limit_check && !plugin->dialog_state->dirty_limit)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->limit_check), FALSE);
        if (plugin->dialog_state) plugin->dialog_state->syncing_ui = FALSE;
        return;
    }
    
    GVariant *value = NULL;
    g_variant_get(result, "(v)", &value);
    g_variant_unref(result);
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
            g_debug("ChargeControlEndThreshold = %d", limit);
            if (plugin->dialog_state && plugin->dialog_state->limit_check && !plugin->dialog_state->dirty_limit)
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(plugin->dialog_state->limit_check), (limit == 80));
        }
        g_variant_unref(value);
    }
    if (plugin->dialog_state) plugin->dialog_state->syncing_ui = FALSE;
}

/* ========== Уведомления ========== */

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

/* ========== Настройки ========== */

static void load_settings(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    XfconfChannel *channel = xfconf_channel_get(CONFIG_CHANNEL);
    if (!channel) { g_warning("Failed to get xfconf channel"); return; }
    plugin->hide_icon = xfconf_channel_get_bool(channel, CONFIG_PROPERTY_PREFIX "/hide_icon", FALSE);
    plugin->hide_text = xfconf_channel_get_bool(channel, CONFIG_PROPERTY_PREFIX "/hide_text", FALSE);
    plugin->hide_notifications = xfconf_channel_get_bool(channel, CONFIG_PROPERTY_PREFIX "/hide_notifications", FALSE);
}

static void save_settings(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    XfconfChannel *channel = xfconf_channel_get(CONFIG_CHANNEL);
    if (!channel) { g_warning("Failed to get xfconf channel"); return; }
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

/* ========== Обновление отображения ========== */

static void update_profile_display(AsusdBatteryPlugin *plugin, gboolean should_notify) {
    if (!plugin) return;
    
    /* Проверяем, загружены ли профили */
    if (!plugin->profiles || plugin->profiles->len == 0) {
        g_debug("update_profile_display: No profiles loaded, creating fallback");
        create_fallback_profiles(plugin);
    }
    
    const gchar *profile = plugin->current_profile ? plugin->current_profile : "balanced";
    /* Если профиль unknown, показываем balanced */
    if (g_strcmp0(profile, "unknown") == 0) {
        profile = "balanced";
        g_debug("update_profile_display: profile was 'unknown', using 'balanced'");
    }
    
    g_debug("=== update_profile_display ===");
    g_debug("  should_notify = %d", should_notify);
    g_debug("  hide_notifications = %d", plugin->hide_notifications);
    g_debug("  profile = '%s'", profile);
    g_debug("  last_displayed_profile = '%s'", plugin->last_displayed_profile ? plugin->last_displayed_profile : "NULL");
    
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
        /* Показываем balanced по умолчанию */
        gtk_label_set_text(GTK_LABEL(plugin->label), _("Balanced"));
        gtk_image_set_from_icon_name(GTK_IMAGE(plugin->image), "battery-good-symbolic", GTK_ICON_SIZE_MENU);
    }
    
    if (profile_changed && profile && g_strcmp0(profile, "unknown") != 0 && can_send_notification(plugin)) {
        g_debug("  >>> SENDING NOTIFICATION for profile: %s", profile);
        gchar *display_name = g_strdup(profile);
        if (display_name[0] >= 'a' && display_name[0] <= 'z') display_name[0] = g_ascii_toupper(display_name[0]);
        const gchar *icon = get_profile_icon(plugin, profile);
        gchar *subtitle = g_strdup_printf(_("Current profile: %s"), display_name);
        send_notification(_("Performance profile changed"), subtitle, FALSE, icon);
        g_free(subtitle);
        g_free(display_name);
    }
    g_debug("=== end update_profile_display ===");
}

/* ========== UI Callbacks ========== */

static void on_button_clicked(GtkWidget *widget, AsusdBatteryPlugin *plugin) {
    if (!plugin || !widget) return;
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

static void on_profile_selected(GtkMenuItem *item, AsusdBatteryPlugin *plugin) {
    if (!item || !plugin) return;
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) return;
    gchar *profile = (gchar *)g_object_get_data(G_OBJECT(item), "profile");
    if (!profile) return;
    asusd_set_profile_async(plugin, profile, (GAsyncReadyCallback)on_set_profile_done, plugin);
}

static void on_set_profile_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin*)user_data;
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error) {
        g_warning("Failed to set profile: %s", error->message);
        if (!plugin->hide_notifications)
            send_notification(_("Error changing profile"), _("Failed to set profile via ASUSD"), TRUE, "emblem-readonly");
        g_error_free(error);
        return;
    }
    if (result) g_variant_unref(result);
    g_debug("Profile set successfully, waiting for property change signal");
}

static gboolean on_button_press(GtkWidget *widget, GdkEventButton *event, AsusdBatteryPlugin *plugin) {
    if (!plugin || !widget) return FALSE;
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

static void on_menu_configure(GtkMenuItem *item, AsusdBatteryPlugin *plugin) { if (!item || !plugin) return; create_settings_dialog(plugin); }
static void on_menu_about(GtkMenuItem *item, AsusdBatteryPlugin *plugin) { if (!item || !plugin) return; create_about_dialog(plugin); }

/* ========== Диалоги ========== */

static void on_dialog_destroy(GtkWidget *widget, AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    if (plugin->settings_dialog_open) {
        g_debug("on_dialog_destroy: settings dialog closed");
        plugin->settings_dialog_open = FALSE;
        plugin->saving_settings = FALSE;
    }
    if (plugin->dialog_state) { g_free(plugin->dialog_state); plugin->dialog_state = NULL; }
}

static void create_about_dialog(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    GtkWidget *dialog = gtk_dialog_new_with_buttons(_("About ASUS Battery Plugin"),
                                        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(plugin->plugin))),
                                        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                        _("_Close"), GTK_RESPONSE_CLOSE, NULL);
    if (!dialog) return;
    gtk_window_set_icon_name(GTK_WINDOW(dialog), "dialog-information");
    gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 300, 180);
    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    if (!content_area) { gtk_widget_destroy(dialog); return; }
    gtk_container_set_border_width(GTK_CONTAINER(content_area), 15);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add(GTK_CONTAINER(content_area), vbox);
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), _("<b>ASUS Battery</b>"));
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);
    label = gtk_label_new(_("Version 1.0 (async)"));
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);
    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), separator, FALSE, FALSE, 5);
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), _("<small>Позволяет переключаться между режимами производительности.</small>"));
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);
    label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(label), _("<small><b>Authors:</b> Deepseek, ChatGPT and korn3r</small>"));
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show_all(dialog);
}

static void on_close_button_clicked(GtkButton *button, GtkWidget *dialog) {
    g_debug("=== on_close_button_clicked: closing dialog ===");
    gtk_widget_destroy(dialog);
}

static void on_any_setting_changed(GtkWidget *widget, AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->dialog_state) return;
    if (plugin->dialog_state->syncing_ui) return;
    if (!plugin->settings_dialog_open) { g_debug("on_any_setting_changed: dialog is closed"); return; }
    g_debug("on_any_setting_changed: setting changed");
    SettingsDialogState *state = plugin->dialog_state;
    if (widget == state->check_ac) { state->dirty_ac_enabled = TRUE; g_debug("  dirty_ac_enabled = TRUE"); }
    else if (widget == state->check_battery) { state->dirty_battery_enabled = TRUE; g_debug("  dirty_battery_enabled = TRUE"); }
    else if (widget == state->combo_ac) { state->dirty_ac_profile = TRUE; g_debug("  dirty_ac_profile = TRUE"); }
    else if (widget == state->combo_battery) { state->dirty_battery_profile = TRUE; g_debug("  dirty_battery_profile = TRUE"); }
    else if (widget == state->limit_check) { state->dirty_limit = TRUE; g_debug("  dirty_limit = TRUE"); }
}

static void on_hide_toggle(GtkToggleButton *toggle_button, AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
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

static void on_apply_clicked(GtkButton *button, AsusdBatteryPlugin *plugin) {
    if (!plugin || !plugin->settings_dialog_open) return;
    g_debug("=== on_apply_clicked: Apply button clicked ===");
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
        g_debug("  ASUSD not available, cannot apply settings");
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
    
    SettingsApplyContext *ctx = g_new0(SettingsApplyContext, 1);
    ctx->plugin = plugin;
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
        g_debug("  No ASUSD changes detected");
        save_settings(plugin);
        settings_dialog_reset_dirty(plugin);
        if (!plugin->hide_notifications)
            send_notification(_("No changes"), _("Settings are already up to date"), FALSE, "emblem-default");
        g_free(ctx->new_ac_profile); g_free(ctx->new_battery_profile); g_free(ctx);
        return;
    }
    
    g_debug("  Starting apply with %d steps", ctx->total_steps);
    plugin->saving_settings = TRUE;
    apply_next_setting(ctx);
}

static void apply_next_setting(SettingsApplyContext *ctx) {
    if (!ctx || !ctx->plugin) return;
    AsusdBatteryPlugin *plugin = ctx->plugin;
    if (ctx->current_step >= ctx->total_steps || ctx->has_errors) {
        on_settings_apply_complete(ctx);
        return;
    }
    g_debug("  Applying step %d/%d", ctx->current_step + 1, ctx->total_steps);
    
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
            g_warning("  Failed to find enum for profile: %s", ctx->new_ac_profile);
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
            g_warning("  Failed to find enum for profile: %s", ctx->new_battery_profile);
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

static void on_settings_apply_step_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    SettingsApplyContext *ctx = (SettingsApplyContext*)user_data;
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error) {
        g_warning("  Failed to apply setting: %s", error->message);
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

static void on_settings_apply_complete(SettingsApplyContext *ctx) {
    AsusdBatteryPlugin *plugin = ctx->plugin;
    plugin->saving_settings = FALSE;
    
    if (ctx->has_errors) {
        g_warning("  One or more settings failed to apply");
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
        g_debug("  Changes applied successfully");
        if (!plugin->hide_notifications)
            send_notification(_("Settings applied"), _("Power profile settings have been updated"), FALSE, "emblem-system");
    }
    
    g_free(ctx->new_ac_profile); g_free(ctx->new_battery_profile);
    if (ctx->error_messages) g_strfreev(ctx->error_messages);
    g_free(ctx);
}

static void on_one_shot_clicked(GtkButton *button, GtkWidget *dialog) {
    GtkWidget *message_dialog;
    gint response;
    AsusdBatteryPlugin *plugin = g_object_get_data(G_OBJECT(dialog), "plugin");
    if (!plugin) { g_warning("on_one_shot_clicked: plugin is NULL"); return; }
    
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
        if (!plugin->asusd_proxy) { g_warning("ASUSD proxy not available"); return; }
        if (limit_enabled_in_dialog) {
            g_debug("on_one_shot_clicked: applying 80%% limit before one-shot");
            asusd_set_property_async(plugin, "ChargeControlEndThreshold", g_variant_new_byte(80), NULL, NULL);
        }
        asusd_call_async(plugin, "OneShotFullCharge", NULL, (GAsyncReadyCallback)on_one_shot_done, plugin);
    }
}

static void on_one_shot_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin*)user_data;
    GError *error = NULL;
    GDBusProxy *proxy = G_DBUS_PROXY(source);
    GVariant *result = g_dbus_proxy_call_finish(proxy, res, &error);
    if (error) {
        gchar *error_message = g_strdup(error->message);
        g_warning("Failed to call OneShotFullCharge: %s", error_message);
        g_error_free(error);
        if (!plugin->hide_notifications)
            send_notification(_("Error"), _("Failed to start one-shot full charge"), TRUE, "dialog-error");
        g_free(error_message);
    } else {
        if (result) g_variant_unref(result);
        if (!plugin->hide_notifications)
            send_notification(_("One-shot full charge started"), _("The battery will charge to 100%% once."), FALSE, "battery-full-symbolic");
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
    if (plugin->settings_dialog_open && plugin->dialog_state && plugin->dialog_state->dialog) {
        g_debug("create_settings_dialog: dialog already open");
        gtk_window_present(GTK_WINDOW(plugin->dialog_state->dialog));
        return;
    }
    if (plugin->dialog_state) { g_free(plugin->dialog_state); plugin->dialog_state = NULL; }
    
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
    
    if (!plugin) return;
    g_debug("=== create_settings_dialog: OPENING ===");
    plugin->settings_dialog_open = TRUE;
    plugin->saving_settings = FALSE;
    plugin->dialog_state = g_new0(SettingsDialogState, 1);
    SettingsDialogState *state = plugin->dialog_state;
    settings_dialog_reset_dirty(plugin);
    
    /* Проверяем, загружены ли профили */
    if (!plugin->profiles || plugin->profiles->len == 0) {
        g_debug("create_settings_dialog: No profiles loaded, creating fallback");
        create_fallback_profiles(plugin);
    }
    
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) {
        g_debug("ASUSD not available, trying to initialize...");
        asusd_init_async(plugin);
    }
    
    dialog = gtk_dialog_new_with_buttons(_("Power Profile Settings"),
                                        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(plugin->plugin))),
                                        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                        NULL, NULL);
    if (!dialog) { g_warning("Failed to create settings dialog"); plugin->settings_dialog_open = FALSE; g_free(plugin->dialog_state); plugin->dialog_state = NULL; return; }
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
    
    /* Auto switch profiles */
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
    
    /* On Battery */
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
    
    /* Battery charge limit */
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
    
    /* One-shot full charge */
    one_shot_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(limit_vbox), one_shot_hbox, FALSE, FALSE, 0);
    one_shot_label = gtk_label_new(_("Charge battery to 100% once:"));
    gtk_box_pack_start(GTK_BOX(one_shot_hbox), one_shot_label, FALSE, FALSE, 0);
    one_shot_button = gtk_button_new_with_label(_("Start"));
    g_signal_connect(G_OBJECT(one_shot_button), "clicked", G_CALLBACK(on_one_shot_clicked), dialog);
    gtk_box_pack_start(GTK_BOX(one_shot_hbox), one_shot_button, FALSE, FALSE, 0);
    
    /* Profile names and icons */
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
    
    /* Display options */
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
    g_debug("=== create_settings_dialog: DIALOG SHOWN ===");
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
    if (!plugin_data) { g_warning("Failed to allocate memory"); return; }
    plugin_data->plugin = plugin;
    plugin_data->current_profile = g_strdup("balanced");
    plugin_data->asusd_state = ASUSD_STATE_UNAVAILABLE;
    plugin_data->hide_icon = FALSE;
    plugin_data->hide_text = FALSE;
    plugin_data->hide_notifications = FALSE;
    plugin_data->is_on_ac = TRUE;
    plugin_data->last_notification_time = 0;
    plugin_data->settings_dialog_open = FALSE;
    plugin_data->saving_settings = FALSE;
    plugin_data->dialog_state = NULL;
    plugin_data->asusd_proxy = NULL;
    plugin_data->upower_proxy = NULL;
    plugin_data->connection = NULL;
    plugin_data->asusd_init_retry_count = 0;
    plugin_data->init_load_state = 0;
    plugin_data->pending_calls = 0;
    plugin_data->processing_ops = FALSE;
    plugin_data->cancellable = g_cancellable_new();
    plugin_data->operation_queue = g_queue_new();
    load_settings(plugin_data);
    
    /* Создаем fallback профили сразу, чтобы они были доступны */
    create_fallback_profiles(plugin_data);
    
    plugin_data->button = gtk_button_new();
    if (!plugin_data->button) { g_warning("Failed to create button"); g_free(plugin_data); return; }
    gtk_button_set_relief(GTK_BUTTON(plugin_data->button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click(plugin_data->button, FALSE);
    gtk_widget_set_tooltip_text(plugin_data->button, _("Manage performance profile"));
    plugin_data->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_add(GTK_CONTAINER(plugin_data->button), plugin_data->box);
    plugin_data->image = gtk_image_new_from_icon_name("battery-good-symbolic", GTK_ICON_SIZE_MENU);
    gtk_box_pack_start(GTK_BOX(plugin_data->box), plugin_data->image, FALSE, FALSE, 0);
    plugin_data->label = gtk_label_new(NULL);
    gtk_box_pack_start(GTK_BOX(plugin_data->box), plugin_data->label, FALSE, FALSE, 0);
    
    /* Обновляем отображение с начальным профилем */
    update_profile_display(plugin_data, FALSE);
    
    asusd_init_async(plugin_data);
    
    g_signal_connect(G_OBJECT(plugin_data->button), "clicked", G_CALLBACK(on_button_clicked), plugin_data);
    g_signal_connect(G_OBJECT(plugin_data->button), "button-press-event", G_CALLBACK(on_button_press), plugin_data);
    gtk_container_add(GTK_CONTAINER(plugin), plugin_data->button);
    gtk_widget_show_all(plugin_data->button);
    create_upower_proxy_async(plugin_data);
    g_object_set_data_full(G_OBJECT(plugin), "plugin_data", plugin_data, asusd_battery_plugin_free);
}

static void asusd_battery_plugin_free(gpointer user_data) {
    AsusdBatteryPlugin *plugin = (AsusdBatteryPlugin *)user_data;
    if (!plugin) return;
    asusd_cleanup(plugin);
    if (plugin->dialog_state) { g_free(plugin->dialog_state); plugin->dialog_state = NULL; }
    g_free(plugin->current_profile);
    g_free(plugin->auto_switch_ac_profile);
    g_free(plugin->auto_switch_battery_profile);
    g_free(plugin->last_displayed_profile);
    g_free(plugin);
}