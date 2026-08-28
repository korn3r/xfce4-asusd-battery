/* src/utils.c */
#include "utils.h"
#include "plugin.h"
#include "profile-manager.h"
#include "settings-dialog.h"
#include "config.h"
#include "debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libxfce4util/libxfce4util.h>

/* ========== Вспомогательные функции для работы с references ========== */

AsusdBatteryPlugin* get_plugin_ref(gpointer user_data) {
    if (!user_data) return NULL;
    if (!G_IS_OBJECT(user_data)) return NULL;
    if (!ASUSD_IS_BATTERY_PLUGIN(user_data)) return NULL;
    
    AsusdBatteryPlugin *plugin = ASUSD_BATTERY_PLUGIN(user_data);
    if (plugin->is_disposing) {
        g_object_unref(user_data);
        return NULL;
    }
    return g_object_ref(ASUSD_BATTERY_PLUGIN(user_data));
}

AsusdBatteryPlugin* async_call_context_get_plugin_ref(AsyncCallContext *ctx) {
    if (!ctx) return NULL;
    GObject *obj = g_weak_ref_get(&ctx->plugin_ref);
    if (!obj) return NULL;
    if (!ASUSD_IS_BATTERY_PLUGIN(obj)) {
        g_object_unref(obj);
        return NULL;
    }
    AsusdBatteryPlugin *plugin = ASUSD_BATTERY_PLUGIN(obj);
    if (plugin->is_disposing) {
        g_object_unref(obj);
        return NULL;
    }
    return ASUSD_BATTERY_PLUGIN(obj);
}

gboolean is_dialog_valid(AsusdBatteryPlugin *plugin, guint dialog_id) {
    if (!plugin || !plugin->dialog_state || plugin->is_disposing) return FALSE;
    return plugin->dialog_state->dialog_id == dialog_id;
}

const gchar* get_profile_icon(AsusdBatteryPlugin *plugin, const gchar *profile) {
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

gboolean can_send_notification(AsusdBatteryPlugin *plugin) {
    if (!plugin) return FALSE;
    time_t current_time = time(NULL);
    if (current_time - plugin->last_notification_time < 2) return FALSE;
    plugin->last_notification_time = current_time;
    return TRUE;
}

void send_notification(const gchar *message, const gchar *subtitle, gboolean is_error, const gchar *icon) {
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
    GError *error = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error)) {
        DEBUG_WARN("xfce4-asusd-battery: Failed to send notification: %s", error ? error->message : "unknown");
        g_error_free(error);
    }
}

void create_about_dialog(AsusdBatteryPlugin *plugin) {
    if (!plugin || plugin->is_disposing) return;
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

const char* asusd_enum_to_default_name(guint32 enum_val) {
    switch (enum_val) {
        case 0: return "balanced";
        case 1: return "performance";
        case 2: return "quiet";
        default: return "unknown";
    }
}

void init_i18n(void) {
    bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);
}
