# ============================================================================
# xfce4-asusd-battery - Makefile
# ============================================================================

CC = gcc
PKG_DEPS = glib-2.0 gtk+-3.0 libxfce4panel-2.0 libxfce4util-1.0 libxfconf-0 gio-2.0
CFLAGS = -Wall -O2 -fPIC `pkg-config --cflags $(PKG_DEPS)` \
         -DLOCALEDIR=\"/usr/share/locale\" \
         -DGETTEXT_PACKAGE=\"xfce4-asusd-battery\" \
         -Iinclude
LDFLAGS = -shared `pkg-config --libs $(PKG_DEPS)`
TARGET = libxfce4-asusd-battery.so
PLUGIN_NAME = xfce4-asusd-battery
SOURCES = src/main.c src/utils.c src/profile-manager.c src/asusd-client.c src/settings-dialog.c

# ========== Определение дистрибутива и путей ==========
DISTRO := $(shell grep -oP '^ID=\K\w+' /etc/os-release 2>/dev/null || echo "unknown")
ARCH := $(shell uname -m)
DEB_MULTIARCH := $(shell dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null)

DESKTOP_DIR = /usr/share/xfce4/panel/plugins
ifeq ($(filter debian ubuntu,$(DISTRO)),$(DISTRO))
    ifneq ($(DEB_MULTIARCH),)
        PLUGIN_DIR = /usr/lib/$(DEB_MULTIARCH)/xfce4/panel/plugins
    else
        PLUGIN_DIR = /usr/lib/$(ARCH)-linux-gnu/xfce4/panel/plugins
    endif
else ifeq ($(DISTRO), arch)
    PLUGIN_DIR = /usr/lib/xfce4/panel/plugins
else
    ifeq ($(ARCH), x86_64)
        PLUGIN_DIR = /usr/lib64/xfce4/panel/plugins
    else
        PLUGIN_DIR = /usr/lib/xfce4/panel/plugins
    endif
endif

LOCALE_DIR = /usr/share/locale
LANGUAGES = ru

# ========== Проверка SELinux ==========
SELINUX_ENABLED := $(shell command -v getenforce >/dev/null 2>&1 && getenforce 2>/dev/null | grep -q "Enforcing\|Permissive" && echo "yes" || echo "no")

# ========== Проверка наличия msgfmt ==========
HAVE_MSGFMT := $(shell command -v msgfmt >/dev/null 2>&1 && echo "yes" || echo "no")

# ========== Проверка уже скомпилированных переводов ==========
MO_FILES_EXIST := $(shell find locale -name "*.mo" 2>/dev/null | head -1)
MO_EXISTS := $(if $(MO_FILES_EXIST),yes,no)

# ========== Цели ==========
all: $(TARGET) translations

$(TARGET): $(SOURCES) | include
	@echo "Building $(TARGET)..."
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) $(SOURCES)
	@echo "Build complete: $(TARGET)"

debug: $(SOURCES)
	$(CC) -Wall -O0 -g `pkg-config --cflags $(PKG_DEPS)` \
		-DLOCALEDIR=\"/usr/share/locale\" \
		-DGETTEXT_PACKAGE=\"xfce4-asusd-battery\" \
		-Iinclude \
		`pkg-config --libs $(PKG_DEPS)` \
		-o $(PLUGIN_NAME)-debug $(SOURCES)

include:
	mkdir -p include

# ========== Сборка переводов ==========
translations:
	@if [ -z "$(LANGUAGES)" ]; then \
		echo "No languages specified, skipping translations"; \
	elif [ "$(HAVE_MSGFMT)" != "yes" ]; then \
		echo "msgfmt not found, skipping translations (install gettext to build translations)"; \
	elif [ "$(MO_EXISTS)" = "yes" ]; then \
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

# ========== Установка ==========
install: all
	@echo "Installing plugin..."
	mkdir -p $(DESTDIR)$(PLUGIN_DIR)
	mkdir -p $(DESTDIR)$(DESKTOP_DIR)
	install -m 755 $(TARGET) $(DESTDIR)$(PLUGIN_DIR)/
	@if [ -f $(PLUGIN_NAME).desktop ]; then \
		install -m 644 $(PLUGIN_NAME).desktop $(DESTDIR)$(DESKTOP_DIR)/; \
	else \
		echo "  Warning: $(PLUGIN_NAME).desktop not found"; \
	fi
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
	fi
	@if [ "$(SELINUX_ENABLED)" = "yes" ]; then \
		echo "SELinux is enabled, restoring contexts..."; \
		if command -v restorecon >/dev/null 2>&1; then \
			echo "  Plugin library..."; \
			restorecon $(DESTDIR)$(PLUGIN_DIR)/$(TARGET) 2>/dev/null || echo "  Warning: Cannot restore SELinux context for plugin (ignored)"; \
			echo "  Desktop file..."; \
			restorecon $(DESTDIR)$(DESKTOP_DIR)/$(PLUGIN_NAME).desktop 2>/dev/null || echo "  Warning: Cannot restore SELinux context for desktop file (ignored)"; \
			if [ -d locale ] && [ -n "$(LANGUAGES)" ]; then \
				for lang_dir in locale/*; do \
					if [ -d "$$lang_dir" ]; then \
						lang=$$(basename "$$lang_dir"); \
						restorecon $(DESTDIR)$(LOCALE_DIR)/$$lang/LC_MESSAGES/$(PLUGIN_NAME).mo 2>/dev/null || true; \
					fi; \
				done; \
			fi; \
			echo "  Done."; \
		else \
			echo "  restorecon not found, skipping SELinux contexts"; \
		fi; \
	else \
		echo "SELinux not enabled or not found, skipping contexts"; \
	fi
	@echo ""
	@echo "============================================================"
	@echo "  xfce4-asusd-battery installed successfully!"
	@echo "  Plugin installed to: $(PLUGIN_DIR)/$(TARGET)"
	@echo "  Desktop file installed to: $(DESKTOP_DIR)/$(PLUGIN_NAME).desktop"
	@echo "============================================================"
	@echo ""
	@echo "Restart panel with: xfce4-panel -r"

# ========== Удаление ==========
uninstall:
	@echo "Uninstalling plugin from system..."
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
	@echo ""
	@echo "Uninstall complete. Restart panel with: xfce4-panel -r"

# ========== Очистка ==========
clean:
	rm -f $(TARGET) $(PLUGIN_NAME)-debug
	rm -rf locale
	@echo "Clean complete"

# ========== Информация ==========
info:
	@echo "============================================================"
	@echo "  xfce4-asusd-battery build information"
	@echo "============================================================"
	@echo "  Distro: $(DISTRO)"
	@echo "  Architecture: $(ARCH)"
	@echo "  Plugin dir: $(PLUGIN_DIR)"
	@echo "  Desktop dir: $(DESKTOP_DIR)"
	@echo "  SELinux: $(SELINUX_ENABLED)"
	@echo "  msgfmt available: $(HAVE_MSGFMT)"
	@echo "  Translations exist: $(MO_EXISTS)"
	@echo "  Languages: $(LANGUAGES)"
	@echo "============================================================"

# ========== Помощь ==========
help:
	@echo "Available targets:"
	@echo "  make         - Build plugin and translations"
	@echo "  make install - Install plugin and translations (sudo)"
	@echo "  make uninstall - Uninstall plugin (sudo)"
	@echo "  make clean   - Remove built files"
	@echo "  make debug   - Build with debug symbols"
	@echo "  make info    - Show build information"
	@echo "  make help    - Show this help"

.PHONY: all install uninstall clean debug info help
