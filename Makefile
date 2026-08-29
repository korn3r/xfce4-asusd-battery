# Makefile
CC = gcc
CFLAGS = -Wall -O2 -fPIC $(shell pkg-config --cflags glib-2.0 gtk+-3.0 libxfce4panel-2.0 libxfce4util-1.0 libxfconf-0 gio-2.0)
LDFLAGS = $(shell pkg-config --libs glib-2.0 gtk+-3.0 libxfce4panel-2.0 libxfce4util-1.0 libxfconf-0 gio-2.0 libnotify) -shared

SOURCES = src/main.c src/utils.c src/profile-manager.c src/asusd-client.c src/settings-dialog.c src/debug.c
HEADERS = include/plugin.h include/utils.h include/profile-manager.h include/asusd-client.h include/settings-dialog.h include/debug.h include/config.h
TARGET = libxfce4-asusd-battery.so

# ========== Определение дистрибутива и путей ==========
DISTRO := $(shell grep -oP '^ID=\K\w+' /etc/os-release 2>/dev/null || echo "unknown")
ARCH := $(shell uname -m)

# Определяем мультиархитектурную папку для Debian/Ubuntu
DEB_MULTIARCH := $(shell dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null)

# Устанавливаем пути в зависимости от дистрибутива
ifeq ($(DISTRO), debian)
    ifneq ($(DEB_MULTIARCH),)
        PLUGIN_DIR = /usr/lib/$(DEB_MULTIARCH)/xfce4/panel/plugins
    else
        PLUGIN_DIR = /usr/lib/$(ARCH)-linux-gnu/xfce4/panel/plugins
    endif
else ifeq ($(DISTRO), ubuntu)
    ifneq ($(DEB_MULTIARCH),)
        PLUGIN_DIR = /usr/lib/$(DEB_MULTIARCH)/xfce4/panel/plugins
    else
        PLUGIN_DIR = /usr/lib/$(ARCH)-linux-gnu/xfce4/panel/plugins
    endif
else ifeq ($(DISTRO), fedora)
    PLUGIN_DIR = /usr/lib64/xfce4/panel/plugins
else ifeq ($(DISTRO), arch)
    PLUGIN_DIR = /usr/lib/xfce4/panel/plugins
else ifeq ($(DISTRO), gentoo)
    PLUGIN_DIR = /usr/lib64/xfce4/panel/plugins
else
    # Fallback для других дистрибутивов
    ifeq ($(ARCH), x86_64)
        PLUGIN_DIR = /usr/lib64/xfce4/panel/plugins
    else
        PLUGIN_DIR = /usr/lib/xfce4/panel/plugins
    endif
endif

# Installation paths
PREFIX ?= /usr
DESTDIR ?=

# SELinux detection
SELINUX_ENABLED := $(shell command -v getenforce >/dev/null 2>&1 && getenforce 2>/dev/null | grep -qi "enforcing\|permissive" && echo "yes" || echo "no")

all: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS)
	@echo "Building $(TARGET)..."
	$(CC) $(CFLAGS) -Iinclude $(LDFLAGS) -o $(TARGET) $(SOURCES)
	@echo "✓ Build complete: $(TARGET)"

clean:
	@echo "Cleaning..."
	rm -f $(TARGET)
	@echo "✓ Clean complete"

install: $(TARGET)
	@echo "Installing $(TARGET)..."
	@echo "  Source:  $(shell pwd)/$(TARGET)"
	@echo "  Target:  $(DESTDIR)$(PLUGIN_DIR)/$(TARGET)"
	@echo "  Size:    $(shell du -h $(TARGET) | cut -f1)"
	@echo "  Distro:  $(DISTRO)"
	@echo "  Arch:    $(ARCH)"
	@echo "  Path:    $(PLUGIN_DIR)"
	mkdir -p $(DESTDIR)$(PLUGIN_DIR)
	install -Dm755 $(TARGET) $(DESTDIR)$(PLUGIN_DIR)/$(TARGET)
	@echo "✓ File installed successfully"
	@if [ "$(SELINUX_ENABLED)" = "yes" ]; then \
		echo "SELinux is enabled, restoring context..."; \
		if command -v restorecon >/dev/null 2>&1; then \
			echo "  Running: restorecon $(DESTDIR)$(PLUGIN_DIR)/$(TARGET)"; \
			restorecon $(DESTDIR)$(PLUGIN_DIR)/$(TARGET) 2>/dev/null || echo "  ⚠ restorecon failed (ignored)"; \
			echo "✓ SELinux context restored"; \
		else \
			echo "  ⚠ restorecon not found, skipping"; \
		fi; \
	else \
		echo "  ℹ SELinux is disabled or not available, skipping restorecon"; \
	fi
	@echo "✓ Installation complete!"
	@echo ""
	@echo "  Plugin installed to:"
	@echo "    $(DESTDIR)$(PLUGIN_DIR)/$(TARGET)"
	@echo ""
	@echo "  To use the plugin:"
	@echo "    1. Restart Xfce Panel: xfce4-panel -r"
	@echo "    2. Add 'ASUS Battery' to your panel"
	@echo ""

uninstall:
	@echo "Uninstalling $(TARGET)..."
	@echo "  Removing: $(DESTDIR)$(PLUGIN_DIR)/$(TARGET)"
	rm -f $(DESTDIR)$(PLUGIN_DIR)/$(TARGET)
	@if [ "$(SELINUX_ENABLED)" = "yes" ] && command -v restorecon >/dev/null 2>&1; then \
		echo "Restoring SELinux context for $(DESTDIR)$(PLUGIN_DIR)..."; \
		restorecon $(DESTDIR)$(PLUGIN_DIR) 2>/dev/null || true; \
	fi
	@echo "✓ Uninstall complete"

info:
	@echo "============================================================"
	@echo "  xfce4-asusd-battery build information"
	@echo "============================================================"
	@echo "  Distro:              $(DISTRO)"
	@echo "  Architecture:        $(ARCH)"
	@echo "  Plugin dir:          $(PLUGIN_DIR)"
	@echo "  SELinux enabled:     $(SELINUX_ENABLED)"
	@echo "  restorecon:          $(shell command -v restorecon >/dev/null 2>&1 && echo "available" || echo "not found")"
	@echo "  PREFIX:              $(PREFIX)"
	@echo "  DESTDIR:             $(DESTDIR)"
	@echo "============================================================"

check:
	@echo "Checking system..."
	@echo "  Distro: $(DISTRO)"
	@echo "  Arch:   $(ARCH)"
	@echo "  Plugin dir: $(PLUGIN_DIR)"
	@if [ -d "$(PLUGIN_DIR)" ]; then \
		echo "  ✓ Plugin directory exists"; \
	else \
		echo "  ✗ Plugin directory does not exist"; \
	fi
	@echo "  SELinux: $(SELINUX_ENABLED)"
	@echo "  restorecon: $(shell command -v restorecon >/dev/null 2>&1 && echo "available" || echo "not found")"

.PHONY: all clean install uninstall info check
