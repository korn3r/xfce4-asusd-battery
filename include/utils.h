#pragma once

#include "plugin.h"

/* Получение плагина с refcount */
AsusdBatteryPlugin* get_plugin_ref(gpointer user_data);

/* Получение плагина из AsyncCallContext */
AsusdBatteryPlugin* async_call_context_get_plugin_ref(AsyncCallContext *ctx);

/* Проверка валидности диалога по ID */
gboolean is_dialog_valid(AsusdBatteryPlugin *plugin, guint dialog_id);

/* Получение иконки для профиля */
const gchar* get_profile_icon(AsusdBatteryPlugin *plugin, const gchar *profile);

/* Проверка возможности отправки уведомления */
gboolean can_send_notification(AsusdBatteryPlugin *plugin);

/* Отправка уведомления */
void send_notification(const gchar *message, const gchar *subtitle, gboolean is_error, const gchar *icon);

/* Создание диалога "О программе" */
void create_about_dialog(AsusdBatteryPlugin *plugin);

/* Определение имени профиля из enum */
const char* asusd_enum_to_default_name(guint32 enum_val);
