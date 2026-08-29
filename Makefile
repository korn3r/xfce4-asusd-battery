# Makefile
CC = gcc
CFLAGS = -Wall -O2 -fPIC $(shell pkg-config --cflags glib-2.0 gtk+-3.0 libxfce4panel-2.0 libxfce4util-1.0 libxfconf-0 gio-2.0)
LDFLAGS = $(shell pkg-config --libs glib-2.0 gtk+-3.0 libxfce4panel-2.0 libxfce4util-1.0 libxfconf-0 gio-2.0 libnotify) -shared

# НЕ определяем LOCALEDIR и GETTEXT_PACKAGE здесь - они в config.h
SOURCES = src/main.c src/utils.c src/profile-manager.c src/asusd-client.c src/settings-dialog.c src/debug.c
HEADERS = include/plugin.h include/utils.h include/profile-manager.h include/asusd-client.h include/settings-dialog.h include/debug.h include/config.h
TARGET = libxfce4-asusd-battery.so

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	$(CC) $(CFLAGS) -Iinclude $(LDFLAGS) -o $(TARGET) $(SOURCES)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/lib/xfce4/panel/plugins/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/xfce4/panel/plugins/$(TARGET)

.PHONY: all clean install uninstall
