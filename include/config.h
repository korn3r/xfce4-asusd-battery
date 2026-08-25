/* include/config.h */
#pragma once

#define GETTEXT_PACKAGE "xfce4-asusd-battery"
#define LOCALEDIR "/usr/share/locale"

/* ASUSD D-Bus константы */
#define ASUSD_BUS_NAME "xyz.ljones.Asusd"
#define ASUSD_OBJECT_PATH "/xyz/ljones"
#define ASUSD_INTERFACE "xyz.ljones.Platform"
#define DBUS_PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"
#define ASUSD_TIMEOUT_MS 5000

/* Xfconf */
#define CONFIG_CHANNEL "xfce4-asusd-battery"
#define CONFIG_PROPERTY_PREFIX "/plugins/xfce4-asusd-battery"
