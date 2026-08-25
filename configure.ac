dnl xfce4-asusd-battery - configure.ac

AC_PREREQ([2.68])
AC_INIT([xfce4-asusd-battery], [1.0.0])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR([build-aux])

AM_INIT_AUTOMAKE([1.11 foreign subdir-objects tar-ustar])
AM_MAINTAINER_MODE([enable])

dnl Checks for programs
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PROG_MAKE_SET
LT_INIT([disable-static])

dnl Checks for Xfce
XDT_I18N([ru])
XDT_CHECK_PACKAGE([GTK], [gtk+-3.0], [3.0])
XDT_CHECK_PACKAGE([LIBXFCE4PANEL], [libxfce4panel-2.0], [4.12])
XDT_CHECK_PACKAGE([LIBXFCE4UTIL], [libxfce4util-1.0], [4.12])
XDT_CHECK_PACKAGE([XFCONF], [libxfconf-0], [4.12])
XDT_CHECK_PACKAGE([GIO], [gio-2.0], [2.40])
XDT_CHECK_PACKAGE([GIO_UNIX], [gio-unix-2.0], [2.40])
XDT_CHECK_PACKAGE([GLIB], [glib-2.0], [2.40])

dnl Define localedir with guard
AH_VERBATIM([LOCALEDIR_GUARD], [
#ifndef LOCALEDIR
#define LOCALEDIR "@localedir@"
#endif
])

dnl Set LOCALEDIR
AC_DEFINE_UNQUOTED([LOCALEDIR], ["$(datadir)/locale"], [Locale directory])

dnl GLib version macros - undefine before defining to avoid warnings
AH_VERBATIM([GLIB_VERSION_GUARD], [
#ifdef GLIB_VERSION_MAX_ALLOWED
#undef GLIB_VERSION_MAX_ALLOWED
#endif
#ifdef GLIB_VERSION_MIN_REQUIRED
#undef GLIB_VERSION_MIN_REQUIRED
#endif
])

dnl Set GLib version macros
AC_DEFINE([GLIB_VERSION_MAX_ALLOWED], [GLIB_VERSION_2_40], [Maximum GLib version])
AC_DEFINE([GLIB_VERSION_MIN_REQUIRED], [GLIB_VERSION_2_40], [Minimum GLib version])

dnl Checks for header files
AC_CHECK_HEADERS([stdlib.h string.h time.h unistd.h])

dnl Checks for functions
AC_CHECK_FUNCS([strdup strndup])

dnl Variables
PLUGIN_CFLAGS="$GTK_CFLAGS $LIBXFCE4PANEL_CFLAGS $LIBXFCE4UTIL_CFLAGS $XFCONF_CFLAGS $GIO_CFLAGS $GIO_UNIX_CFLAGS $GLIB_CFLAGS"
PLUGIN_LIBS="$GTK_LIBS $LIBXFCE4PANEL_LIBS $LIBXFCE4UTIL_LIBS $XFCONF_LIBS $GIO_LIBS $GIO_UNIX_LIBS $GLIB_LIBS"

AC_SUBST([PLUGIN_CFLAGS])
AC_SUBST([PLUGIN_LIBS])

dnl Output files
AC_CONFIG_FILES([
    Makefile
    po/Makefile.in
])

AC_OUTPUT

echo
echo "xfce4-asusd-battery configuration:"
echo "  Prefix: ${prefix}"
echo "  Compiler: ${CC}"
echo "  NLS enabled: ${enable_nls}"
echo
