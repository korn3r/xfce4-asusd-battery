## Process this file with automake to produce Makefile.in
## xfce4-asusd-battery - Makefile.am

SUBDIRS = po

# Panel plugin
lib_LTLIBRARIES = libxfce4_asusd_battery.la

libxfce4_asusd_battery_la_SOURCES = \
    src/main.c

libxfce4_asusd_battery_la_CFLAGS = \
    $(PLUGIN_CFLAGS) \
    -I$(top_srcdir) \
    -DG_LOG_DOMAIN=\"xfce4-asusd-battery\"

libxfce4_asusd_battery_la_LIBADD = $(PLUGIN_LIBS)

libxfce4_asusd_battery_la_LDFLAGS = \
    -module \
    -avoid-version \
    -no-undefined \
    -export-dynamic

# Installation directories
plugindir = $(libdir)/xfce4/panel/plugins
plugin_DATA = libxfce4_asusd_battery.la

# Desktop file
desktopdir = $(datadir)/xfce4/panel/plugins
desktop_DATA = xfce4-asusd-battery.desktop

# Translation files
EXTRA_DIST = \
    $(desktop_DATA) \
    autogen.sh \
    src/main.c \
    README.md \
    COPYING \
    AUTHORS \
    ChangeLog \
    INSTALL \
    NEWS

# Cleanup
CLEANFILES = \
    *.la \
    *.so \
    *.a \
    *.lo \
    *.o \
    *.deps

DISTCLEANFILES = \
    Makefile \
    config.log \
    config.status \
    autom4te.cache

# Extra files to include in distribution
DISTCHECK_CONFIGURE_FLAGS = \
    --enable-maintainer-mode

# Extra files to clean
maintainer-clean-local:
	rm -rf $(top_srcdir)/autom4te.cache
	rm -rf $(top_srcdir)/build-aux
	rm -f $(top_srcdir)/configure
	rm -f $(top_srcdir)/config.h.in
	rm -f $(top_srcdir)/Makefile.in
	rm -f $(top_srcdir)/ltmain.sh
	rm -f $(top_srcdir)/aclocal.m4
	rm -rf $(top_srcdir)/m4
