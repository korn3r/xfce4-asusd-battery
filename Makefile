# Makefile
CC = gcc
CFLAGS = -Wall -O2 -fPIC $(shell pkg-config --cflags glib-2.0 gtk+-3.0 libxfce4panel-2.0 libxfce4util-1.0 libxfconf-0 gio-2.0)
LDFLAGS = $(shell pkg-config --libs glib-2.0 gtk+-3.0 libxfce4panel-2.0 libxfce4util-1.0 libxfconf-0 gio-2.0 libnotify) -shared

# ========== Определение дистрибутива и путей ==========
DISTRO := $(shell grep -oP '^ID=\K\w+' /etc/os-release 2>/dev/null || echo "unknown")
ARCH := $(shell uname -m)
DEB_MULTIARCH := $(shell dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null)

# Определяем пути в зависимости от дистрибутива
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
    ifeq ($(ARCH), x86_64)
        PLUGIN_DIR = /usr/lib64/xfce4/panel/plugins
    else
        PLUGIN_DIR = /usr/lib/xfce4/panel/plugins
    endif
endif

# Пути для .desktop и локалей
DESKTOP_DIR = /usr/share/xfce4/panel/plugins
LOCALE_DIR = /usr/share/locale

# ========== Список языков ==========
LANGUAGES = ru de fr zh_CN es it pl hi fa ar be fi da sv nb sr ko mi ms vi ja

# Installation paths
PREFIX ?= /usr
DESTDIR ?=

# SELinux detection
SELINUX_ENABLED := $(shell command -v getenforce >/dev/null 2>&1 && getenforce 2>/dev/null | grep -qi "enforcing\|permissive" && echo "yes" || echo "no")

# Проверка наличия msgfmt
HAVE_MSGFMT := $(shell command -v msgfmt >/dev/null 2>&1 && echo "yes" || echo "no")

SOURCES = src/main.c src/utils.c src/profile-manager.c src/asusd-client.c src/settings-dialog.c src/debug.c
HEADERS = include/plugin.h include/utils.h include/profile-manager.h include/asusd-client.h include/settings-dialog.h include/debug.h include/config.h
TARGET = libxfce4-asusd-battery.so
PLUGIN_NAME = xfce4-asusd-battery

all: $(TARGET) translations

$(TARGET): $(SOURCES) $(HEADERS)
	@echo "Building $(TARGET)..."
	$(CC) $(CFLAGS) -Iinclude $(LDFLAGS) -o $(TARGET) $(SOURCES)
	@echo "✓ Build complete: $(TARGET)"

# ========== Сборка переводов ==========
translations:
	@if [ -z "$(LANGUAGES)" ]; then \
		echo "No languages specified, skipping translations"; \
	elif [ "$(HAVE_MSGFMT)" != "yes" ]; then \
		echo "msgfmt not found, skipping translations (install gettext to build translations)"; \
	elif [ -d locale ] && [ -n "$$(ls locale/*/LC_MESSAGES/*.mo 2>/dev/null)" ]; then \
		echo "Translations already built, skipping"; \
	else \
		echo "Building translations..."; \
		mkdir -p locale; \
		for lang in $(LANGUAGES); do \
			if [ -f "po/$$lang.po" ]; then \
				mkdir -p "locale/$$lang/LC_MESSAGES"; \
				msgfmt -o "locale/$$lang/LC_MESSAGES/$(PLUGIN_NAME).mo" "po/$$lang.po" 2>/dev/null && echo "  $$lang done" || echo "  $$lang failed"; \
			else \
				echo "  $$lang skipped (po/$$lang.po not found)"; \
			fi; \
		done; \
	fi

# ========== Принудительная сборка переводов ==========
force-translations:
	@echo "Forcing translation build..."
	@if [ "$(HAVE_MSGFMT)" != "yes" ]; then \
		echo "msgfmt not found, cannot build translations (install gettext)"; \
		exit 1; \
	fi
	@mkdir -p locale
	@for lang in $(LANGUAGES); do \
		if [ -f "po/$$lang.po" ]; then \
			mkdir -p "locale/$$lang/LC_MESSAGES"; \
			msgfmt -o "locale/$$lang/LC_MESSAGES/$(PLUGIN_NAME).mo" "po/$$lang.po" 2>/dev/null && echo "  $$lang done" || echo "  $$lang failed"; \
		else \
			echo "  $$lang skipped (po/$$lang.po not found)"; \
		fi; \
	done

# ========== Установка ==========
install: $(TARGET) translations
	@echo "Installing $(TARGET)..."
	@echo "  Source:  $(shell pwd)/$(TARGET)"
	@echo "  Target:  $(DESTDIR)$(PLUGIN_DIR)/$(TARGET)"
	@echo "  Desktop: $(DESTDIR)$(DESKTOP_DIR)/$(PLUGIN_NAME).desktop"
	@echo "  Size:    $(shell du -h $(TARGET) | cut -f1)"
	@echo "  Distro:  $(DISTRO)"
	@echo "  Arch:    $(ARCH)"
	@echo "  Path:    $(PLUGIN_DIR)"
	
	# Установка плагина
	mkdir -p $(DESTDIR)$(PLUGIN_DIR)
	install -Dm755 $(TARGET) $(DESTDIR)$(PLUGIN_DIR)/$(TARGET)
	@echo "✓ Plugin installed"
	
	# Установка .desktop файла
	mkdir -p $(DESTDIR)$(DESKTOP_DIR)
	if [ -f "$(PLUGIN_NAME).desktop" ]; then \
		install -m 644 $(PLUGIN_NAME).desktop $(DESTDIR)$(DESKTOP_DIR)/; \
		echo "✓ Desktop file installed"; \
	else \
		echo "⚠ Desktop file not found, skipping"; \
	fi
	
	# Установка переводов
	@if [ -d locale ] && [ -n "$(LANGUAGES)" ]; then \
		echo "Installing translations..."; \
		for lang_dir in locale/*; do \
			if [ -d "$$lang_dir" ]; then \
				lang=$$(basename "$$lang_dir"); \
				target_mo="$(DESTDIR)$(LOCALE_DIR)/$$lang/LC_MESSAGES/$(PLUGIN_NAME).mo"; \
				source_mo="locale/$$lang/LC_MESSAGES/$(PLUGIN_NAME).mo"; \
				if [ -f "$$source_mo" ]; then \
					mkdir -p $(DESTDIR)$(LOCALE_DIR)/$$lang/LC_MESSAGES; \
					install -m 644 "$$source_mo" "$$target_mo"; \
					echo "  $$lang installed"; \
				fi; \
			fi; \
		done; \
	else \
		echo "  ℹ No translations found, skipping"; \
	fi
	
	# SELinux
	@if [ "$(SELINUX_ENABLED)" = "yes" ]; then \
		echo "SELinux is enabled, restoring context..."; \
		if command -v restorecon >/dev/null 2>&1; then \
			echo "  Plugin: restorecon $(DESTDIR)$(PLUGIN_DIR)/$(TARGET)"; \
			restorecon $(DESTDIR)$(PLUGIN_DIR)/$(TARGET) 2>/dev/null || echo "  ⚠ restorecon failed (ignored)"; \
			if [ -f "$(DESTDIR)$(DESKTOP_DIR)/$(PLUGIN_NAME).desktop" ]; then \
				echo "  Desktop: restorecon $(DESTDIR)$(DESKTOP_DIR)/$(PLUGIN_NAME).desktop"; \
				restorecon $(DESTDIR)$(DESKTOP_DIR)/$(PLUGIN_NAME).desktop 2>/dev/null || true; \
			fi; \
			for lang_dir in locale/*; do \
				if [ -d "$$lang_dir" ]; then \
					lang=$$(basename "$$lang_dir"); \
					restorecon $(DESTDIR)$(LOCALE_DIR)/$$lang/LC_MESSAGES/$(PLUGIN_NAME).mo 2>/dev/null || true; \
				fi; \
			done; \
			echo "✓ SELinux contexts restored"; \
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
	@echo "  Desktop file installed to:"
	@echo "    $(DESTDIR)$(DESKTOP_DIR)/$(PLUGIN_NAME).desktop"
	@echo ""
	@echo "  To use the plugin:"
	@echo "    1. Restart Xfce Panel: xfce4-panel -r"
	@echo "    2. Add 'ASUS Battery' to your panel"
	@echo ""

# ========== Установка только переводов ==========
install-translations:
	@echo "Installing translations only..."
	@if [ -d locale ] && [ -n "$(LANGUAGES)" ]; then \
		for lang_dir in locale/*; do \
			if [ -d "$$lang_dir" ]; then \
				lang=$$(basename "$$lang_dir"); \
				target_mo="$(DESTDIR)$(LOCALE_DIR)/$$lang/LC_MESSAGES/$(PLUGIN_NAME).mo"; \
				source_mo="locale/$$lang/LC_MESSAGES/$(PLUGIN_NAME).mo"; \
				if [ -f "$$source_mo" ]; then \
					mkdir -p $(DESTDIR)$(LOCALE_DIR)/$$lang/LC_MESSAGES; \
					install -m 644 "$$source_mo" "$$target_mo"; \
					echo "  $$lang installed"; \
				fi; \
			fi; \
		done; \
	else \
		echo "  No translations found in locale/ directory"; \
		echo "  Run 'make translations' or 'make force-translations' first"; \
	fi

# ========== Удаление ==========
uninstall:
	@echo "Uninstalling $(TARGET)..."
	rm -f $(DESTDIR)$(PLUGIN_DIR)/$(TARGET)
	rm -f $(DESTDIR)$(DESKTOP_DIR)/$(PLUGIN_NAME).desktop
	@if [ -d locale ] && [ -n "$(LANGUAGES)" ]; then \
		for lang_dir in locale/*; do \
			if [ -d "$$lang_dir" ]; then \
				lang=$$(basename "$$lang_dir"); \
				rm -f $(DESTDIR)$(LOCALE_DIR)/$$lang/LC_MESSAGES/$(PLUGIN_NAME).mo; \
				echo "  $$lang removed"; \
			fi; \
		done; \
	fi
	@if [ "$(SELINUX_ENABLED)" = "yes" ] && command -v restorecon >/dev/null 2>&1; then \
		echo "Restoring SELinux contexts for directories..."; \
		restorecon $(DESTDIR)$(PLUGIN_DIR) 2>/dev/null || true; \
		restorecon $(DESTDIR)$(DESKTOP_DIR) 2>/dev/null || true; \
	fi
	@echo "✓ Uninstall complete"

# ========== Очистка ==========
clean:
	@echo "Cleaning..."
	rm -f $(TARGET)
	rm -rf locale
	@echo "✓ Clean complete"

# ========== Информация ==========
info:
	@echo "============================================================"
	@echo "  xfce4-asusd-battery build information"
	@echo "============================================================"
	@echo "  Distro:              $(DISTRO)"
	@echo "  Architecture:        $(ARCH)"
	@echo "  Plugin dir:          $(PLUGIN_DIR)"
	@echo "  Desktop dir:         $(DESKTOP_DIR)"
	@echo "  Locale dir:          $(LOCALE_DIR)"
	@echo "  SELinux enabled:     $(SELINUX_ENABLED)"
	@echo "  msgfmt available:    $(HAVE_MSGFMT)"
	@echo "  Languages:           $(LANGUAGES)"
	@echo "============================================================"

# ========== Проверка зависимостей ==========
check:
	@echo "Checking dependencies..."
	@for pkg in glib-2.0 gtk+-3.0 libxfce4panel-2.0 libxfce4util-1.0 libxfconf-0 gio-2.0; do \
		if pkg-config --exists $$pkg 2>/dev/null; then \
			echo "  ✓ $$pkg"; \
		else \
			echo "  ✗ $$pkg - NOT FOUND"; \
		fi; \
	done
	@echo ""
	@echo "Checking translations..."
	@if [ "$(HAVE_MSGFMT)" = "yes" ]; then \
		echo "  ✓ msgfmt available"; \
		for lang in $(LANGUAGES); do \
			if [ -f "po/$$lang.po" ]; then \
				echo "  ✓ $$lang.po"; \
			else \
				echo "  ✗ $$lang.po - NOT FOUND"; \
			fi; \
		done; \
	else \
		echo "  ✗ msgfmt NOT available (install gettext)"; \
	fi

.PHONY: all clean install uninstall install-translations translations force-translations info check
