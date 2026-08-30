/* src/asusd-client.c */
#include "asusd-client.h"
#include "profile-manager.h"
#include "utils.h"
#include "plugin.h"
#include "settings-dialog.h"
#include "config.h"
#include "debug.h"

#include <gio/gio.h>
#include <libxfce4util/libxfce4util.h>

#define ASUSD_BUS_NAME "xyz.ljones.Asusd"
#define ASUSD_OBJECT_PATH "/xyz/ljones"
#define ASUSD_INTERFACE "xyz.ljones.Platform"
#define DBUS_PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"
#define ASUSD_TIMEOUT_MS 5000

/* ========== Forward declarations ========== */
void on_asusd_proxy_created(GObject *source, GAsyncResult *res, gpointer user_data);
void on_upower_proxy_created(GObject *source, GAsyncResult *res, gpointer user_data);
void on_proxy_properties_changed(GDBusProxy *proxy, GVariant *changed_properties,
                                 GStrv invalidated_properties, gpointer user_data);
void on_property_set_done(GObject *source, GAsyncResult *res, gpointer user_data);
void on_get_property_done(GObject *source, GAsyncResult *res, gpointer user_data);
void on_profile_choices_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_current_profile_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_limit_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_ac_switch_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_ac_profile_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_battery_switch_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_battery_profile_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_one_shot_done(GObject *source, GAsyncResult *res, gpointer user_data);
void process_next_operation(AsusdBatteryPlugin *plugin);
void on_dialog_property_loaded(GObject *source, GAsyncResult *res, gpointer user_data);

/* ========== AsyncCallContext API ========== */

AsyncCallContext* async_call_context_new(AsusdBatteryPlugin *plugin,
                                         const char *method_name,
                                         GVariant *value,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data,
                                         GDestroyNotify destroy_notify) {
    AsyncCallContext *ctx = g_new0(AsyncCallContext, 1);
    g_weak_ref_init(&ctx->plugin_ref, G_OBJECT(plugin));
    ctx->method_name = g_strdup(method_name);
    if (value) ctx->value = g_variant_ref(value);
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->destroy_notify = destroy_notify;
    ctx->ref_count = 1;
    ctx->dialog_id = 0;
    ctx->is_dialog_callback = FALSE;
    return ctx;
}

void async_call_context_free(AsyncCallContext *ctx) {
    if (!ctx) return;
    if (g_atomic_int_dec_and_test(&ctx->ref_count)) {
        g_weak_ref_clear(&ctx->plugin_ref);
        g_free(ctx->method_name);
        if (ctx->value) g_variant_unref(ctx->value);
        if (ctx->destroy_notify && ctx->user_data) ctx->destroy_notify(ctx->user_data);
        g_free(ctx);
    }
}

void async_call_context_ref(AsyncCallContext *ctx) {
    if (ctx) g_atomic_int_inc(&ctx->ref_count);
}

void async_call_context_unref(AsyncCallContext *ctx) {
    if (ctx) async_call_context_free(ctx);
}

AsusdBatteryPlugin* async_call_context_get_plugin_ref(AsyncCallContext *ctx) {
    if (!ctx) return NULL;
    return g_weak_ref_get(&ctx->plugin_ref);
}

/* ========== Создание прокси ========== */

void create_asusd_proxy_async(AsusdBatteryPlugin *plugin) {
    if (!plugin || plugin->is_disposing) return;
    if (g_cancellable_is_cancelled(plugin->cancellable)) return;
    
    DEBUG_TRACE("xfce4-asusd-battery: Creating ASUSD proxy asynchronously");
    
    g_dbus_proxy_new_for_bus(
        G_BUS_TYPE_SYSTEM,
        G_DBUS_PROXY_FLAGS_NONE,
        NULL,
        ASUSD_BUS_NAME,
        ASUSD_OBJECT_PATH,
        ASUSD_INTERFACE,
        plugin->cancellable,
        (GAsyncReadyCallback)on_asusd_proxy_created,
        g_object_ref(plugin)
    );
}

void on_asusd_proxy_created(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = get_plugin_ref(user_data);
    if (!plugin) {
        DEBUG_TRACE("xfce4-asusd-battery: Plugin destroyed, discarding proxy callback");
        return;
    }
    
    if (g_cancellable_is_cancelled(plugin->cancellable)) {
        DEBUG_TRACE("xfce4-asusd-battery: Operation cancelled, discarding proxy callback");
        g_object_unref(plugin);
        return;
    }
    
    GError *error = NULL;
    plugin->asusd_proxy = g_dbus_proxy_new_for_bus_finish(res, &error);
    if (error) {
        DEBUG_WARN("xfce4-asusd-battery: Failed to create proxy: %s", error->message);
        g_error_free(error);
        plugin->asusd_state = ASUSD_STATE_UNAVAILABLE;
        
        /* Clear reconnecting flag BEFORE scheduling retry */
        plugin->reconnecting = FALSE;
        
        if (plugin->asusd_retry_timeout_id == 0 && !plugin->is_disposing)
            plugin->asusd_retry_timeout_id = g_timeout_add_seconds(5, asusd_retry_init, plugin);
        g_object_unref(plugin);
        return;
    }
    if (!plugin->asusd_proxy) {
        DEBUG_WARN("xfce4-asusd-battery: Proxy creation returned NULL");
        plugin->asusd_state = ASUSD_STATE_UNAVAILABLE;
        plugin->reconnecting = FALSE;
        g_object_unref(plugin);
        return;
    }
    
    DEBUG_INFO("xfce4-asusd-battery: Connected to ASUSD D-Bus service");
    DEBUG_TRACE("xfce4-asusd-battery: Proxy created successfully");
    plugin->connection = g_dbus_proxy_get_connection(plugin->asusd_proxy);
    if (plugin->connection) g_object_ref(plugin->connection);
    
    g_signal_connect(plugin->asusd_proxy, "g-properties-changed",
                    G_CALLBACK(on_proxy_properties_changed), plugin);
    
    gchar *owner = g_dbus_proxy_get_name_owner(plugin->asusd_proxy);
    DEBUG_TRACE("xfce4-asusd-battery: Current name owner = %s", owner ? owner : "NULL");
    
    if (owner) {
        g_free(owner);
        DEBUG_TRACE("xfce4-asusd-battery: Owner exists, loading initial data...");
        plugin->asusd_state = ASUSD_STATE_AVAILABLE;
        plugin->init_load_state = 0;
        plugin->reconnecting = FALSE;
        asusd_get_property_async(plugin, "PlatformProfileChoices",
                                (GAsyncReadyCallback)on_profile_choices_loaded, plugin);
    } else {
        g_free(owner);
        DEBUG_TRACE("xfce4-asusd-battery: No owner yet, will retry");
        plugin->asusd_state = ASUSD_STATE_UNAVAILABLE;
        plugin->reconnecting = FALSE;
        create_fallback_profiles(plugin);
        if (plugin->asusd_retry_timeout_id == 0 && !plugin->is_disposing)
            plugin->asusd_retry_timeout_id = g_timeout_add_seconds(2, asusd_retry_init, plugin);
    }
    
    g_object_unref(plugin);
}

/* ========== Создание UPower прокси ========== */

void create_upower_proxy_async(AsusdBatteryPlugin *plugin) {
    if (!plugin || plugin->is_disposing) return;
    if (g_cancellable_is_cancelled(plugin->cancellable)) return;
    
    DEBUG_TRACE("xfce4-asusd-battery: Creating UPower proxy asynchronously");
    g_dbus_proxy_new_for_bus(
        G_BUS_TYPE_SYSTEM,
        G_DBUS_PROXY_FLAGS_NONE,
        NULL,
        "org.freedesktop.UPower",
        "/org/freedesktop/UPower",
        "org.freedesktop.UPower",
        plugin->cancellable,
        (GAsyncReadyCallback)on_upower_proxy_created,
        g_object_ref(plugin)
    );
}

void on_upower_proxy_created(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = get_plugin_ref(user_data);
    if (!plugin) {
        DEBUG_TRACE("xfce4-asusd-battery: Plugin destroyed, discarding proxy callback");
        return;
    }
    
    if (g_cancellable_is_cancelled(plugin->cancellable)) {
        DEBUG_TRACE("xfce4-asusd-battery: Operation cancelled, discarding proxy callback");
        g_object_unref(plugin);
        return;
    }
    
    GError *error = NULL;
    plugin->upower_proxy = g_dbus_proxy_new_for_bus_finish(res, &error);
    if (error) {
        DEBUG_WARN("xfce4-asusd-battery: Failed to create UPower proxy: %s", error->message);
        g_error_free(error);
        g_object_unref(plugin);
        return;
    }
    DEBUG_TRACE("xfce4-asusd-battery: UPower proxy created successfully");
    g_signal_connect(plugin->upower_proxy, "g-properties-changed",
                    G_CALLBACK(on_proxy_properties_changed), plugin);
    
    g_object_unref(plugin);
}

/* ========== Сигналы прокси ========== */

void on_proxy_properties_changed(GDBusProxy *proxy,
                                 GVariant *changed_properties,
                                 GStrv invalidated_properties,
                                 gpointer user_data) {
    AsusdBatteryPlugin *plugin = get_plugin_ref(user_data);
    if (!plugin || !changed_properties || plugin->is_disposing) {
        if (plugin) g_object_unref(plugin);
        return;
    }
    
    if (g_cancellable_is_cancelled(plugin->cancellable)) {
        DEBUG_TRACE("xfce4-asusd-battery: Operation cancelled, discarding properties changed callback");
        g_object_unref(plugin);
        return;
    }
    
    if (plugin->saving_settings) {
        DEBUG_TRACE("xfce4-asusd-battery: on_proxy_properties_changed: saving_settings in progress, skipping UI updates");
        g_object_unref(plugin);
        return;
    }
    
    DEBUG_TRACE("xfce4-asusd-battery: === on_proxy_properties_changed ===");
    
    if (proxy == plugin->asusd_proxy) {
        DEBUG_TRACE("xfce4-asusd-battery:   From ASUSD proxy");
        GVariantIter iter;
        gchar *key;
        GVariant *value;
        g_variant_iter_init(&iter, changed_properties);
        while (g_variant_iter_next(&iter, "{sv}", &key, &value)) {
            DEBUG_TRACE("xfce4-asusd-battery:   changed property: %s", key);
            if (g_strcmp0(key, "PlatformProfile") == 0) {
                guint32 enum_val;
                g_variant_get(value, "u", &enum_val);
                const gchar *name = profile_name_from_enum(plugin, enum_val);
                if (!plugin->current_profile || g_strcmp0(plugin->current_profile, name) != 0) {
                    g_free(plugin->current_profile);
                    plugin->current_profile = g_strdup(name);
                    DEBUG_INFO("xfce4-asusd-battery: Profile changed to: %s (via D-Bus signal)", name);
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
                        GtkTreeIter tree_iter;
                        gboolean found = FALSE;
                        gchar *profile_name = NULL;
                        gint index = 0;
                        if (gtk_tree_model_get_iter_first(model, &tree_iter)) {
                            do {
                                gtk_tree_model_get(model, &tree_iter, 0, &profile_name, -1);
                                if (g_strcmp0(profile_name, name) == 0) { found = TRUE; break; }
                                g_free(profile_name);
                                index++;
                            } while (gtk_tree_model_iter_next(model, &tree_iter));
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
                        GtkTreeIter tree_iter;
                        gboolean found = FALSE;
                        gchar *profile_name = NULL;
                        gint index = 0;
                        if (gtk_tree_model_get_iter_first(model, &tree_iter)) {
                            do {
                                gtk_tree_model_get(model, &tree_iter, 0, &profile_name, -1);
                                if (g_strcmp0(profile_name, name) == 0) { found = TRUE; break; }
                                g_free(profile_name);
                                index++;
                            } while (gtk_tree_model_iter_next(model, &tree_iter));
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
        DEBUG_TRACE("xfce4-asusd-battery:   From UPower proxy");
        GVariantDict dict;
        g_variant_dict_init(&dict, changed_properties);
        GVariant *value = g_variant_dict_lookup_value(&dict, "OnBattery", NULL);
        if (value) {
            gboolean on_battery = g_variant_get_boolean(value);
            plugin->is_on_ac = !on_battery;
            DEBUG_TRACE("xfce4-asusd-battery:   OnBattery = %d, is_on_ac = %d", on_battery, plugin->is_on_ac);
            g_variant_unref(value);
        }
        g_variant_dict_clear(&dict);
    }
    
    g_object_unref(plugin);
}

/* ========== Асинхронные операции с очередью ========== */

void asusd_queue_operation(AsusdBatteryPlugin *plugin, const char *method,
                           GVariant *parameters, GAsyncReadyCallback callback,
                           gpointer user_data) {
    if (!plugin) return;
    AsyncCallContext *ctx = async_call_context_new(plugin, method, parameters, callback, user_data, NULL);
    g_queue_push_tail(plugin->operation_queue, ctx);
    if (!plugin->processing_ops) process_next_operation(plugin);
}

void process_next_operation(AsusdBatteryPlugin *plugin) {
    if (!plugin || plugin->is_disposing) return;
    
    /* Don't process during reconnect */
    if (plugin->reconnecting) {
        DEBUG_TRACE("xfce4-asusd-battery: Reconnecting in progress, deferring operations");
        plugin->processing_ops = FALSE;
        return;
    }
    
    if (g_cancellable_is_cancelled(plugin->cancellable)) {
        DEBUG_TRACE("xfce4-asusd-battery: Operation cancelled, clearing queue");
        g_queue_free_full(plugin->operation_queue, (GDestroyNotify)async_call_context_free);
        plugin->operation_queue = g_queue_new();
        plugin->processing_ops = FALSE;
        return;
    }
    
    if (g_queue_is_empty(plugin->operation_queue)) { 
        plugin->processing_ops = FALSE; 
        return; 
    }
    if (!plugin->asusd_proxy) {
        DEBUG_TRACE("xfce4-asusd-battery: Proxy not available");
        plugin->processing_ops = FALSE;
        return;
    }
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) {
        DEBUG_TRACE("xfce4-asusd-battery: Not available (state=%d)", plugin->asusd_state);
        plugin->processing_ops = FALSE;
        if (plugin->asusd_retry_timeout_id == 0)
            plugin->asusd_retry_timeout_id = g_timeout_add_seconds(2, asusd_retry_init, plugin);
        return;
    }
    
    AsyncCallContext *ctx = g_queue_pop_head(plugin->operation_queue);
    if (!ctx) { 
        plugin->processing_ops = FALSE; 
        return; 
    }
    
    plugin->processing_ops = TRUE;
    plugin->pending_calls++;
    
    DEBUG_TRACE("xfce4-asusd-battery: Processing operation: %s", ctx->method_name);
    DEBUG_TRACE("xfce4-asusd-battery: D-Bus call -> %s on %s", ctx->method_name, ASUSD_BUS_NAME);
    if (ctx->value) {
        gchar *params_str = g_variant_print(ctx->value, TRUE);
        DEBUG_TRACE("xfce4-asusd-battery: D-Bus params: %s", params_str);
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

void on_property_set_done(GObject *source,
                          GAsyncResult *res,
                          gpointer user_data)
{
    AsyncCallContext *ctx = (AsyncCallContext *)user_data;
    if (!ctx)
        return;

    AsusdBatteryPlugin *plugin =
        async_call_context_get_plugin_ref(ctx);

    if (!plugin || plugin->is_disposing) {
        DEBUG_TRACE("xfce4-asusd-battery: Plugin destroyed or disposing, discarding callback");

        if (plugin)
            g_object_unref(plugin);

        async_call_context_unref(ctx);
        return;
    }

    if (g_cancellable_is_cancelled(plugin->cancellable)) {
        DEBUG_TRACE("xfce4-asusd-battery: Operation cancelled");

        if (ctx->callback) {
            GAsyncReadyCallback callback = ctx->callback;
            callback(source, NULL, ctx->user_data);
        }

        g_object_unref(plugin);
        async_call_context_unref(ctx);
        return;
    }

    plugin->pending_calls--;

    GError *error = NULL;
    GDBusProxy *proxy = G_DBUS_PROXY(source);
    GVariant *result = g_dbus_proxy_call_finish(proxy, res, &error);

    if (error) {
        if (g_error_matches(error,
                            G_DBUS_ERROR,
                            G_DBUS_ERROR_DISCONNECTED)) {

            DEBUG_WARN("xfce4-asusd-battery: ASUSD disconnected, reconnecting...");

            if (plugin->reconnecting ||
                plugin->asusd_retry_timeout_id > 0) {

                DEBUG_WARN(
                    "xfce4-asusd-battery: Reconnection already in progress, skipping duplicate");

                g_error_free(error);
                g_object_unref(plugin);
                async_call_context_unref(ctx);
                return;
            }

            /* Re-queue the operation before reconnect */
            g_queue_push_head(plugin->operation_queue, ctx);
            async_call_context_ref(ctx);

            plugin->reconnecting = TRUE;
            g_error_free(error);

            asusd_init_async(plugin);

            g_object_unref(plugin);
            return;
        }

        DEBUG_WARN("xfce4-asusd-battery: Operation failed: %s",
                   error->message);

        /* For dialog callbacks, pass NULL to indicate failure */
        if (ctx->is_dialog_callback && ctx->callback) {
            GAsyncReadyCallback callback = ctx->callback;
            callback(source, NULL, ctx->user_data);
        } else if (ctx->callback) {
            GAsyncReadyCallback callback = ctx->callback;
            callback(source, res, ctx->user_data);
        }

        g_error_free(error);
    } else {
        DEBUG_INFO(
            "xfce4-asusd-battery: Operation completed successfully: %s",
            ctx->method_name ? ctx->method_name : "unknown");

        if (ctx->callback) {
            GAsyncReadyCallback callback = ctx->callback;
            callback(source, res, ctx->user_data);
        }

        if (result)
            g_variant_unref(result);
    }

    g_object_unref(plugin);
    async_call_context_unref(ctx);
}

void asusd_call_async(AsusdBatteryPlugin *plugin, const char *method,
                      GVariant *parameters, GAsyncReadyCallback callback,
                      gpointer user_data) {
    if (!plugin || plugin->is_disposing) return;
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) {
        DEBUG_TRACE("xfce4-asusd-battery: Call %s queued (state=%d)", method, plugin->asusd_state);
    }
    asusd_queue_operation(plugin, method, parameters, callback, user_data);
}

/* ========== Callback for async Get property ========== */

void on_get_property_done(GObject *source,
                         GAsyncResult *res,
                         gpointer user_data)
{
    AsyncCallContext *ctx = (AsyncCallContext *)user_data;
    if (!ctx)
        return;

    AsusdBatteryPlugin *plugin =
        async_call_context_get_plugin_ref(ctx);

    if (!plugin || plugin->is_disposing) {
        DEBUG_TRACE(
            "xfce4-asusd-battery: Plugin destroyed, discarding get property callback");

        /* Если это dialog callback, освобождаем outer ctx */
        if (ctx->is_dialog_callback && ctx->user_data) {
            AsyncCallContext *outer_ctx = (AsyncCallContext*)ctx->user_data;
            async_call_context_free(outer_ctx);
        }

        if (plugin)
            g_object_unref(plugin);

        async_call_context_unref(ctx);
        return;
    }

    if (g_cancellable_is_cancelled(plugin->cancellable)) {
        DEBUG_TRACE(
            "xfce4-asusd-battery: Get property operation cancelled");

        /* Если это dialog callback, освобождаем outer ctx */
        if (ctx->is_dialog_callback && ctx->user_data) {
            AsyncCallContext *outer_ctx = (AsyncCallContext*)ctx->user_data;
            async_call_context_free(outer_ctx);
        }

        g_object_unref(plugin);
        async_call_context_unref(ctx);
        return;
    }

    /* Handle NULL result from failed operation */
    if (!res) {
        DEBUG_TRACE("xfce4-asusd-battery: Get property operation failed with NULL result");
        if (ctx->is_dialog_callback && ctx->user_data) {
            AsyncCallContext *outer_ctx = (AsyncCallContext*)ctx->user_data;
            async_call_context_free(outer_ctx);
        }
        g_object_unref(plugin);
        async_call_context_unref(ctx);
        return;
    }

    if (ctx->callback) {
        GAsyncReadyCallback callback = ctx->callback;
        callback(source, res, ctx->user_data);
    }

    g_object_unref(plugin);
    async_call_context_unref(ctx);
}

/* ========== Работа со свойствами через GDBusConnection ========== */

gboolean asusd_get_property_async(AsusdBatteryPlugin *plugin, const char *property,
                                  GAsyncReadyCallback callback, gpointer user_data) {
    if (!plugin || !property || plugin->is_disposing) {
        DEBUG_TRACE("xfce4-asusd-battery: Invalid parameters, operation not started");
        return FALSE;
    }
    
    if (g_cancellable_is_cancelled(plugin->cancellable)) {
        DEBUG_TRACE("xfce4-asusd-battery: Cancelled, operation not started");
        return FALSE;
    }
    
    if (!plugin->connection) {
        DEBUG_WARN("xfce4-asusd-battery: No connection available, operation not started");
        return FALSE;
    }
    
    DEBUG_TRACE("xfce4-asusd-battery: D-Bus Get property: %s", property);
    
    /* Создаем контекст для безопасной передачи user_data */
    AsyncCallContext *ctx = async_call_context_new(plugin, NULL, NULL, callback, user_data, NULL);
    if (!ctx) {
        DEBUG_WARN("xfce4-asusd-battery: Failed to create context, operation not started");
        return FALSE;
    }
    
    ctx->method_name = g_strdup(property);
    ctx->is_dialog_callback = FALSE;
    
    /* GLib берет на себя управление памятью параметров */
    g_dbus_connection_call(
        plugin->connection,
        ASUSD_BUS_NAME,
        ASUSD_OBJECT_PATH,
        DBUS_PROPERTIES_INTERFACE,
        "Get",
        g_variant_new("(ss)", ASUSD_INTERFACE, property),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE,
        ASUSD_TIMEOUT_MS,
        plugin->cancellable,
        (GAsyncReadyCallback)on_get_property_done,
        ctx
    );
    
    return TRUE;
}

void asusd_set_property_async(AsusdBatteryPlugin *plugin, const char *property,
                              GVariant *value, GAsyncReadyCallback callback,
                              gpointer user_data) {
    if (!plugin || !property || !value || plugin->is_disposing) return;
    if (g_cancellable_is_cancelled(plugin->cancellable)) return;
    
    DEBUG_TRACE("xfce4-asusd-battery: D-Bus Set property: %s", property);
    if (!plugin->connection) {
        DEBUG_WARN("xfce4-asusd-battery: No connection available");
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

void asusd_set_profile_async(AsusdBatteryPlugin *plugin, const gchar *profile_name,
                             GAsyncReadyCallback callback, gpointer user_data) {
    if (!plugin || !profile_name || plugin->is_disposing) return;
    if (plugin->asusd_state != ASUSD_STATE_AVAILABLE) return;
    if (g_cancellable_is_cancelled(plugin->cancellable)) return;
    
    guint32 enum_val = 999;
    if (!profile_enum_from_name(plugin, profile_name, &enum_val)) {
        DEBUG_WARN("xfce4-asusd-battery: Profile %s not found", profile_name);
        return;
    }
    asusd_set_property_async(plugin, "PlatformProfile", g_variant_new_uint32(enum_val), callback, user_data);
}

/* ========== Retry механизм ========== */

gboolean asusd_retry_init(gpointer user_data) {
    AsusdBatteryPlugin *plugin = get_plugin_ref(user_data);
    if (!plugin || plugin->is_disposing) {
        if (plugin) g_object_unref(plugin);
        return G_SOURCE_REMOVE;
    }
    
    plugin->asusd_retry_timeout_id = 0;
    plugin->reconnecting = FALSE;  /* Clear flag before retry */
    DEBUG_TRACE("xfce4-asusd-battery: Retry init attempt %d", plugin->asusd_init_retry_count + 1);
    plugin->asusd_init_retry_count++;
    
    if (g_cancellable_is_cancelled(plugin->cancellable)) {
        DEBUG_TRACE("xfce4-asusd-battery: Retry cancelled");
        g_object_unref(plugin);
        return G_SOURCE_REMOVE;
    }
    
    if (plugin->asusd_proxy) {
        gchar *owner = g_dbus_proxy_get_name_owner(plugin->asusd_proxy);
        if (owner) {
            DEBUG_TRACE("xfce4-asusd-battery: Owner found on retry: %s", owner);
            g_free(owner);
            plugin->asusd_state = ASUSD_STATE_AVAILABLE;
            asusd_get_property_async(plugin, "PlatformProfileChoices",
                                    (GAsyncReadyCallback)on_profile_choices_loaded, plugin);
            g_object_unref(plugin);
            return G_SOURCE_REMOVE;
        }
        g_free(owner);
    }
    asusd_init_async(plugin);
    g_object_unref(plugin);
    return G_SOURCE_REMOVE;
}

void asusd_init_async(AsusdBatteryPlugin *plugin) {
    if (!plugin || plugin->is_disposing) return;
    if (g_cancellable_is_cancelled(plugin->cancellable)) return;
    
    DEBUG_TRACE("xfce4-asusd-battery: Initializing asynchronously...");
    
    /* Очищаем старый proxy, НО НЕ УНИЧТОЖАЕМ ПРОФИЛИ! */
    if (plugin->asusd_proxy) {
        DEBUG_TRACE("xfce4-asusd-battery: Cleaning up old proxy");
        g_signal_handlers_disconnect_by_data(plugin->asusd_proxy, plugin);
        g_clear_object(&plugin->asusd_proxy);
        g_clear_object(&plugin->connection);
    }
    
    /* НЕ УНИЧТОЖАЕМ ПРОФИЛИ! */
    /* Профили уже созданы в create_fallback_profiles() и загружены в load_settings() */
    
    plugin->asusd_state = ASUSD_STATE_CONNECTING;
    create_asusd_proxy_async(plugin);
}

void asusd_cleanup(AsusdBatteryPlugin *plugin) {
    if (!plugin) return;
    
    DEBUG_TRACE("xfce4-asusd-battery: Cleaning up");
    
    /* Clear reconnection flag */
    plugin->reconnecting = FALSE;
    
    if (plugin->cancellable) {
        g_cancellable_cancel(plugin->cancellable);
        g_clear_object(&plugin->cancellable);
    }
    if (plugin->connection) {
        g_clear_object(&plugin->connection);
    }
    if (plugin->asusd_proxy) {
        g_signal_handlers_disconnect_by_data(plugin->asusd_proxy, plugin);
        g_clear_object(&plugin->asusd_proxy);
    }
    if (plugin->upower_proxy) {
        g_signal_handlers_disconnect_by_data(plugin->upower_proxy, plugin);
        g_clear_object(&plugin->upower_proxy);
    }
    if (plugin->profiles) {
        g_ptr_array_free(plugin->profiles, TRUE);
        plugin->profiles = NULL;
    }
    if (plugin->profile_lookup) {
        g_hash_table_destroy(plugin->profile_lookup);
        plugin->profile_lookup = NULL;
    }
    if (plugin->operation_queue) {
        g_queue_free_full(plugin->operation_queue, (GDestroyNotify)async_call_context_free);
        plugin->operation_queue = NULL;
    }
    if (plugin->asusd_retry_timeout_id > 0) {
        g_source_remove(plugin->asusd_retry_timeout_id);
        plugin->asusd_retry_timeout_id = 0;
    }
    plugin->asusd_state = ASUSD_STATE_UNAVAILABLE;
    plugin->processing_ops = FALSE;
    plugin->pending_calls = 0;
    plugin->saving_settings = FALSE;  /* <-- ADD THIS LINE */
}

/* ========== Callbacks для загрузки данных ASUSD ========== */

void on_profile_choices_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = get_plugin_ref(user_data);
    if (!plugin || plugin->is_disposing) {
        if (plugin) g_object_unref(plugin);
        DEBUG_TRACE("xfce4-asusd-battery: Plugin destroyed, discarding profile choices callback");
        return;
    }
    
    if (g_cancellable_is_cancelled(plugin->cancellable)) {
        DEBUG_TRACE("xfce4-asusd-battery: Operation cancelled, discarding profile choices callback");
        g_object_unref(plugin);
        return;
    }
    
    GError *error = NULL;
    
    if (plugin->profiles && plugin->profiles->len > 0) {
        DEBUG_TRACE("xfce4-asusd-battery: Profiles already loaded, skipping");
        g_object_unref(plugin);
        return;
    }
    
    /* Завершаем D-Bus вызов */
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error) {
        if (g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD)) {
            DEBUG_WARN("xfce4-asusd-battery: PlatformProfileChoices not supported, using fallback");
            g_error_free(error);
            create_fallback_profiles(plugin);
            if (!asusd_get_property_async(plugin, "PlatformProfile", 
                                          (GAsyncReadyCallback)on_current_profile_loaded, plugin)) {
                DEBUG_WARN("Failed to start async operation for PlatformProfile");
                g_object_unref(plugin);
                return;
            }
            g_object_unref(plugin);
            return;
        }
        DEBUG_WARN("xfce4-asusd-battery: Failed to get PlatformProfileChoices: %s", error->message);
        g_error_free(error);
        plugin->asusd_state = ASUSD_STATE_UNAVAILABLE;
        if (plugin->asusd_retry_timeout_id == 0 && !plugin->is_disposing)
            plugin->asusd_retry_timeout_id = g_timeout_add_seconds(5, asusd_retry_init, plugin);
        g_object_unref(plugin);
        return;
    }
    
    if (!result) {
        DEBUG_WARN("xfce4-asusd-battery: No result for PlatformProfileChoices");
        create_fallback_profiles(plugin);
        if (!asusd_get_property_async(plugin, "PlatformProfile",
                                      (GAsyncReadyCallback)on_current_profile_loaded, plugin)) {
            DEBUG_WARN("Failed to start async operation for PlatformProfile");
            g_object_unref(plugin);
            return;
        }
        g_object_unref(plugin);
        return;
    }
    
    /* Извлекаем значение из результата */
    GVariant *value = NULL;
    g_variant_get(result, "(v)", &value);
    g_variant_unref(result);
    
    if (!value) {
        DEBUG_WARN("xfce4-asusd-battery: No value in PlatformProfileChoices");
        create_fallback_profiles(plugin);
        if (!asusd_get_property_async(plugin, "PlatformProfile",
                                      (GAsyncReadyCallback)on_current_profile_loaded, plugin)) {
            DEBUG_WARN("Failed to start async operation for PlatformProfile");
            g_object_unref(plugin);
            return;
        }
        g_object_unref(plugin);
        return;
    }
    
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
        GVariant *inner = g_variant_get_variant(value);
        g_variant_unref(value);
        value = inner;
    }
    
    if (!g_variant_is_of_type(value, G_VARIANT_TYPE_ARRAY)) {
        DEBUG_WARN("xfce4-asusd-battery: PlatformProfileChoices not array");
        g_variant_unref(value);
        create_fallback_profiles(plugin);
        if (!asusd_get_property_async(plugin, "PlatformProfile",
                                      (GAsyncReadyCallback)on_current_profile_loaded, plugin)) {
            DEBUG_WARN("Failed to start async operation for PlatformProfile");
            g_object_unref(plugin);
            return;
        }
        g_object_unref(plugin);
        return;
    }
    
    if (!plugin->profiles) {
        plugin->profiles = g_ptr_array_new_with_free_func((GDestroyNotify)profile_settings_free);
    }
    if (!plugin->profile_lookup) {
        plugin->profile_lookup = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);
    }
    
    parse_profile_choices(plugin, value);
    g_variant_unref(value);
    DEBUG_INFO("xfce4-asusd-battery: Loaded %d available performance profiles", plugin->profiles->len);
    
    if (!asusd_get_property_async(plugin, "PlatformProfile",
                                  (GAsyncReadyCallback)on_current_profile_loaded, plugin)) {
        DEBUG_WARN("Failed to start async operation for PlatformProfile");
        g_object_unref(plugin);
        return;
    }
    g_object_unref(plugin);
}

void on_current_profile_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = get_plugin_ref(user_data);
    if (!plugin || plugin->is_disposing) {
        if (plugin) g_object_unref(plugin);
        DEBUG_TRACE("xfce4-asusd-battery: Plugin destroyed, discarding current profile callback");
        return;
    }
    
    if (g_cancellable_is_cancelled(plugin->cancellable)) {
        DEBUG_TRACE("xfce4-asusd-battery: Operation cancelled, discarding current profile callback");
        g_object_unref(plugin);
        return;
    }
    
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error) {
        DEBUG_TRACE("xfce4-asusd-battery: PlatformProfile query failed: %s", error->message);
        g_error_free(error);
        if (!plugin->current_profile || g_strcmp0(plugin->current_profile, "unknown") == 0) {
            g_free(plugin->current_profile);
            plugin->current_profile = g_strdup("balanced");
            plugin->asusd_state = ASUSD_STATE_AVAILABLE;
            update_profile_display(plugin, FALSE);
        }
        g_object_unref(plugin);
        return;
    }
    
    if (!result) {
        DEBUG_TRACE("xfce4-asusd-battery: No result for PlatformProfile");
        if (!plugin->current_profile || g_strcmp0(plugin->current_profile, "unknown") == 0) {
            g_free(plugin->current_profile);
            plugin->current_profile = g_strdup("balanced");
            plugin->asusd_state = ASUSD_STATE_AVAILABLE;
            update_profile_display(plugin, FALSE);
        }
        g_object_unref(plugin);
        return;
    }
    
    GVariant *value = NULL;
    g_variant_get(result, "(v)", &value);
    g_variant_unref(result);
    
    if (!value) {
        DEBUG_TRACE("xfce4-asusd-battery: No value in PlatformProfile");
        if (!plugin->current_profile || g_strcmp0(plugin->current_profile, "unknown") == 0) {
            g_free(plugin->current_profile);
            plugin->current_profile = g_strdup("balanced");
            plugin->asusd_state = ASUSD_STATE_AVAILABLE;
            update_profile_display(plugin, FALSE);
        }
        g_object_unref(plugin);
        return;
    }
    
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
        GVariant *inner = g_variant_get_variant(value);
        g_variant_unref(value);
        value = inner;
    }
    
    if (!g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32)) {
        DEBUG_WARN("xfce4-asusd-battery: PlatformProfile not uint32");
        g_variant_unref(value);
        if (!plugin->current_profile || g_strcmp0(plugin->current_profile, "unknown") == 0) {
            g_free(plugin->current_profile);
            plugin->current_profile = g_strdup("balanced");
            plugin->asusd_state = ASUSD_STATE_AVAILABLE;
            update_profile_display(plugin, FALSE);
        }
        g_object_unref(plugin);
        return;
    }
    
    guint32 enum_val;
    g_variant_get(value, "u", &enum_val);
    g_variant_unref(value);
    const gchar *name = profile_name_from_enum(plugin, enum_val);
    g_free(plugin->current_profile);
    plugin->current_profile = g_strdup(name);
    plugin->asusd_state = ASUSD_STATE_AVAILABLE;
    DEBUG_INFO("xfce4-asusd-battery: Current profile: %s", name);
    update_profile_display(plugin, FALSE);
    
    if (plugin->init_load_state == 0) {
        plugin->init_load_state = 1;
        if (!asusd_get_property_async(plugin, "ChargeControlEndThreshold",
                                      (GAsyncReadyCallback)on_limit_loaded, plugin)) {
            DEBUG_WARN("Failed to start async operation for ChargeControlEndThreshold");
            g_object_unref(plugin);
            return;
        }
    }
    g_object_unref(plugin);
}

void on_limit_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = get_plugin_ref(user_data);
    if (!plugin || plugin->is_disposing) {
        if (plugin) g_object_unref(plugin);
        DEBUG_TRACE("xfce4-asusd-battery: Plugin destroyed, discarding limit callback");
        return;
    }
    
    if (g_cancellable_is_cancelled(plugin->cancellable)) {
        DEBUG_TRACE("xfce4-asusd-battery: Operation cancelled, discarding limit callback");
        g_object_unref(plugin);
        return;
    }
    
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error) {
        DEBUG_TRACE("xfce4-asusd-battery: Failed to get ChargeControlEndThreshold: %s", error->message);
        g_error_free(error);
    } else if (result) {
        GVariant *value = NULL;
        g_variant_get(result, "(v)", &value);
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
                DEBUG_INFO("xfce4-asusd-battery: Battery charge limit: %d%%", limit);
            }
            g_variant_unref(value);
        }
        g_variant_unref(result);
    }
    
    if (!asusd_get_property_async(plugin, "ChangePlatformProfileOnAc",
                                  (GAsyncReadyCallback)on_ac_switch_loaded, plugin)) {
        DEBUG_WARN("Failed to start async operation for ChangePlatformProfileOnAc");
        g_object_unref(plugin);
        return;
    }
    g_object_unref(plugin);
}

void on_ac_switch_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = get_plugin_ref(user_data);
    if (!plugin || plugin->is_disposing) {
        if (plugin) g_object_unref(plugin);
        DEBUG_TRACE("xfce4-asusd-battery: Plugin destroyed, discarding ac switch callback");
        return;
    }
    
    if (g_cancellable_is_cancelled(plugin->cancellable)) {
        DEBUG_TRACE("xfce4-asusd-battery: Operation cancelled, discarding ac switch callback");
        g_object_unref(plugin);
        return;
    }
    
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error) {
        DEBUG_TRACE("xfce4-asusd-battery: Failed to get ChangePlatformProfileOnAc: %s", error->message);
        g_error_free(error);
    } else if (result) {
        GVariant *value = NULL;
        g_variant_get(result, "(v)", &value);
        if (value) {
            if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
                GVariant *inner = g_variant_get_variant(value);
                g_variant_unref(value);
                value = inner;
            }
            if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
                g_variant_get(value, "b", &plugin->auto_switch_ac_enabled);
                DEBUG_INFO("xfce4-asusd-battery: Auto-switch on AC: %s", 
                           plugin->auto_switch_ac_enabled ? "enabled" : "disabled");
            }
            g_variant_unref(value);
        }
        g_variant_unref(result);
    }
    
    if (!asusd_get_property_async(plugin, "PlatformProfileOnAc",
                                  (GAsyncReadyCallback)on_ac_profile_loaded, plugin)) {
        DEBUG_WARN("Failed to start async operation for PlatformProfileOnAc");
        g_object_unref(plugin);
        return;
    }
    g_object_unref(plugin);
}

void on_ac_profile_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = get_plugin_ref(user_data);
    if (!plugin || plugin->is_disposing) {
        if (plugin) g_object_unref(plugin);
        DEBUG_TRACE("xfce4-asusd-battery: Plugin destroyed, discarding ac profile callback");
        return;
    }
    
    if (g_cancellable_is_cancelled(plugin->cancellable)) {
        DEBUG_TRACE("xfce4-asusd-battery: Operation cancelled, discarding ac profile callback");
        g_object_unref(plugin);
        return;
    }
    
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error) {
        DEBUG_TRACE("xfce4-asusd-battery: Failed to get PlatformProfileOnAc: %s", error->message);
        g_error_free(error);
    } else if (result) {
        GVariant *value = NULL;
        g_variant_get(result, "(v)", &value);
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
                DEBUG_INFO("xfce4-asusd-battery: AC profile: %s", name);
            }
            g_variant_unref(value);
        }
        g_variant_unref(result);
    }
    
    if (!asusd_get_property_async(plugin, "ChangePlatformProfileOnBattery",
                                  (GAsyncReadyCallback)on_battery_switch_loaded, plugin)) {
        DEBUG_WARN("Failed to start async operation for ChangePlatformProfileOnBattery");
        g_object_unref(plugin);
        return;
    }
    g_object_unref(plugin);
}

void on_battery_switch_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = get_plugin_ref(user_data);
    if (!plugin || plugin->is_disposing) {
        if (plugin) g_object_unref(plugin);
        DEBUG_TRACE("xfce4-asusd-battery: Plugin destroyed, discarding battery switch callback");
        return;
    }
    
    if (g_cancellable_is_cancelled(plugin->cancellable)) {
        DEBUG_TRACE("xfce4-asusd-battery: Operation cancelled, discarding battery switch callback");
        g_object_unref(plugin);
        return;
    }
    
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error) {
        DEBUG_TRACE("xfce4-asusd-battery: Failed to get ChangePlatformProfileOnBattery: %s", error->message);
        g_error_free(error);
    } else if (result) {
        GVariant *value = NULL;
        g_variant_get(result, "(v)", &value);
        if (value) {
            if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
                GVariant *inner = g_variant_get_variant(value);
                g_variant_unref(value);
                value = inner;
            }
            if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
                g_variant_get(value, "b", &plugin->auto_switch_battery_enabled);
                DEBUG_INFO("xfce4-asusd-battery: Auto-switch on battery: %s", 
                           plugin->auto_switch_battery_enabled ? "enabled" : "disabled");
            }
            g_variant_unref(value);
        }
        g_variant_unref(result);
    }
    
    if (!asusd_get_property_async(plugin, "PlatformProfileOnBattery",
                                  (GAsyncReadyCallback)on_battery_profile_loaded, plugin)) {
        DEBUG_WARN("Failed to start async operation for PlatformProfileOnBattery");
        g_object_unref(plugin);
        return;
    }
    g_object_unref(plugin);
}

void on_battery_profile_loaded(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = get_plugin_ref(user_data);
    if (!plugin || plugin->is_disposing) {
        if (plugin) g_object_unref(plugin);
        DEBUG_TRACE("xfce4-asusd-battery: Plugin destroyed, discarding battery profile callback");
        return;
    }
    
    if (g_cancellable_is_cancelled(plugin->cancellable)) {
        DEBUG_TRACE("xfce4-asusd-battery: Operation cancelled, discarding battery profile callback");
        g_object_unref(plugin);
        return;
    }
    
    GError *error = NULL;
    GVariant *result = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
    if (error) {
        DEBUG_TRACE("xfce4-asusd-battery: Failed to get PlatformProfileOnBattery: %s", error->message);
        g_error_free(error);
    } else if (result) {
        GVariant *value = NULL;
        g_variant_get(result, "(v)", &value);
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
                DEBUG_INFO("xfce4-asusd-battery: Battery profile: %s", name);
            }
            g_variant_unref(value);
        }
        g_variant_unref(result);
    }
    
    if (!plugin->profiles || plugin->profiles->len == 0) {
        DEBUG_TRACE("xfce4-asusd-battery: No profiles loaded after init, creating fallback");
        create_fallback_profiles(plugin);
    }
    
    plugin->asusd_state = ASUSD_STATE_AVAILABLE;
    plugin->asusd_init_retry_count = 0;
    plugin->reconnecting = FALSE;
    update_profile_display(plugin, FALSE);
    DEBUG_INFO("xfce4-asusd-battery: ASUSD initialization completed");
    if (plugin->dialog_state && plugin->dialog_state->dialog)
        settings_dialog_sync_from_asusd(plugin, FALSE);
    
    /* Resume processing queue if there are pending operations */
    if (!g_queue_is_empty(plugin->operation_queue)) {
        DEBUG_TRACE("xfce4-asusd-battery: Resuming operation queue (%u operations)", 
                    g_queue_get_length(plugin->operation_queue));
        process_next_operation(plugin);
    }
    
    g_object_unref(plugin);
}

void on_one_shot_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    AsusdBatteryPlugin *plugin = get_plugin_ref(user_data);
    if (!plugin || plugin->is_disposing) {
        if (plugin) g_object_unref(plugin);
        return;
    }
    
    if (g_cancellable_is_cancelled(plugin->cancellable)) {
        DEBUG_TRACE("xfce4-asusd-battery: Operation cancelled, discarding one-shot callback");
        g_object_unref(plugin);
        return;
    }
    
    GError *error = NULL;
    GDBusProxy *proxy = G_DBUS_PROXY(source);
    GVariant *result = g_dbus_proxy_call_finish(proxy, res, &error);
    if (error) {
        gchar *error_message = g_strdup(error->message);
        DEBUG_WARN("xfce4-asusd-battery: Failed to call OneShotFullCharge: %s", error_message);
        g_error_free(error);
        if (!plugin->hide_notifications)
            send_notification(_("Error"), _("Failed to start one-shot full charge"), TRUE, "dialog-error");
        g_free(error_message);
    } else {
        if (result) g_variant_unref(result);
        DEBUG_INFO("xfce4-asusd-battery: One-shot full charge started");
        if (!plugin->hide_notifications)
            send_notification(_("One-shot full charge started"), _("The battery will charge to 100%% once."), FALSE, "battery-full-symbolic");
    }
    
    g_object_unref(plugin);
}
