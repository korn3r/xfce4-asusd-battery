/* include/config.h */
#ifndef CONFIG_H
#define CONFIG_H

/* ========== Package definitions ========== */

#ifndef VERSION
#define VERSION "1.0.0"
#endif

#ifndef GETTEXT_PACKAGE
#define GETTEXT_PACKAGE "xfce4-asusd-battery"
#endif

#ifndef LOCALEDIR
#define LOCALEDIR "/usr/share/locale"
#endif

/* ========== Xfconf configuration ========== */

#define CONFIG_CHANNEL "xfce4-asusd-battery"
#define CONFIG_PROPERTY_PREFIX "/"

#endif /* CONFIG_H */
