#pragma once

#include "plugin.h"

/* Инициализация ASUSD */
void asusd_init_async(AsusdBatteryPlugin *plugin);

/* Очистка ASUSD (вызывается из dispose) */
void asusd_cleanup(AsusdBatteryPlugin *plugin);

/* Получение свойства через D-Bus */
void asusd_get_property_async(AsusdBatteryPlugin *plugin, const char *property,
                              GAsyncReadyCallback callback, gpointer user_data);

/* Установка свойства через D-Bus */
void asusd_set_property_async(AsusdBatteryPlugin *plugin, const char *property,
                              GVariant *value, GAsyncReadyCallback callback,
                              gpointer user_data);

/* Установка профиля */
void asusd_set_profile_async(AsusdBatteryPlugin *plugin, const gchar *profile_name,
                             GAsyncReadyCallback callback, gpointer user_data);

/* Вызов метода ASUSD */
void asusd_call_async(AsusdBatteryPlugin *plugin, const char *method,
                      GVariant *parameters, GAsyncReadyCallback callback,
                      gpointer user_data);

/* Обработка очереди операций */
void process_next_operation(AsusdBatteryPlugin *plugin);

/* Сигнал изменения свойств */
void on_proxy_properties_changed(GDBusProxy *proxy, GVariant *changed_properties,
                                 GStrv invalidated_properties, gpointer user_data);

/* Создание прокси ASUSD */
void create_asusd_proxy_async(AsusdBatteryPlugin *plugin);

/* Создание прокси UPower */
void create_upower_proxy_async(AsusdBatteryPlugin *plugin);

/* Retry инициализации */
gboolean asusd_retry_init(gpointer user_data);

/* Callback для создания прокси */
void on_asusd_proxy_created(GObject *source, GAsyncResult *res, gpointer user_data);
void on_upower_proxy_created(GObject *source, GAsyncResult *res, gpointer user_data);

/* Callback для D-Bus вызовов */
void on_property_set_done(GObject *source, GAsyncResult *res, gpointer user_data);

/* Callback для загрузки данных ASUSD */
void on_profile_choices_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_current_profile_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_limit_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_ac_switch_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_battery_switch_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_ac_profile_loaded(GObject *source, GAsyncResult *res, gpointer user_data);
void on_battery_profile_loaded(GObject *source, GAsyncResult *res, gpointer user_data);

void on_one_shot_done(GObject *source, GAsyncResult *res, gpointer user_data);
