CC = gcc
PKG_DEPS = glib-2.0 gtk+-3.0 libxfce4panel-2.0 libxfce4util-1.0 libxfconf-0 gio-2.0
CFLAGS = -Wall -O2 -fPIC -DLOCALEDIR=\"/usr/share/locale\" `pkg-config --cflags $(PKG_DEPS)`
LDFLAGS = -shared `pkg-config --libs $(PKG_DEPS)`
TARGET = libxfce4_asusd_battery.so
DEBUG_TARGET = xfce4-asusd-battery-debug
PLUGIN_NAME = xfce4-asusd-battery
SOURCES = src/$(PLUGIN_NAME).c

# ========== Определение дистрибутива и путей ==========
DISTRO := $(shell grep -oP '^ID=\K\w+' /etc/os-release 2>/dev/null || echo "unknown")
ARCH := $(shell uname -m)

# Определяем мультиархитектурную папку для Debian/Ubuntu
DEB_MULTIARCH := $(shell dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null)

# Устанавливаем пути в зависимости от дистрибутива
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
    # Fallback для других дистрибутивов
    ifeq ($(ARCH), x86_64)
        PLUGIN_DIR = /usr/lib64/xfce4/panel/plugins
    else
        PLUGIN_DIR = /usr/lib/xfce4/panel/plugins
    endif
endif

# Пути для локалей
LOCALE_DIR = /usr/share/locale

# ========== Список языков для перевода ==========
LANGUAGES = ru de fr zh_CN es it pl hi fa ar be fi da sv nb sr ko mi ms vi ja

# ========== Проверка SELinux ==========
SELINUX_ENABLED := $(shell command -v getenforce >/dev/null 2>&1 && getenforce 2>/dev/null | grep -q "Enforcing\|Permissive" && echo "yes" || echo "no")

# ========== Проверка наличия msgfmt ==========
HAVE_MSGFMT := $(shell command -v msgfmt >/dev/null 2>&1 && echo "yes" || echo "no")

# ========== Проверка уже скомпилированных переводов ==========
MO_FILES_EXIST := $(shell find locale -name "*.mo" 2>/dev/null | head -1)
MO_EXISTS := $(if $(MO_FILES_EXIST),yes,no)

# ========== Проверка необходимости пересборки переводов ==========
NEED_REBUILD := no
ifneq ($(MO_EXISTS), yes)
    NEED_REBUILD := yes
endif

# ========== Цели сборки ==========
all: $(TARGET) translations

$(TARGET): $(SOURCES)
	mkdir -p src
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) $(SOURCES)

debug: $(SOURCES)
	$(CC) -Wall -O0 -g `pkg-config --cflags $(PKG_DEPS)` \
		`pkg-config --libs $(PKG_DEPS)` \
		-o $(DEBUG_TARGET) $(SOURCES)

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
install: all
	@echo "Installing plugin..."
	mkdir -p $(DESTDIR)$(PLUGIN_DIR)
	mkdir -p $(DESTDIR)$(DESKTOP_DIR)
	install -m 755 $(TARGET) $(DESTDIR)$(PLUGIN_DIR)/
	install -m 644 $(PLUGIN_NAME).desktop $(DESTDIR)$(DESKTOP_DIR)/
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
	@echo "  SELinux: $(if $(filter yes,$(SELINUX_ENABLED)),Enabled,Disabled)"
	@echo "============================================================"
	@echo ""
	@echo "Restart panel with: xfce4-panel -r"

# ========== Установка переводов без сборки ==========
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
	rm -f $(TARGET) $(DEBUG_TARGET)
	rm -rf locale

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

# ========== Проверка зависимостей ==========
check:
	@echo "Checking dependencies..."
	@for pkg in $(PKG_DEPS); do \
		if pkg-config --exists $$pkg 2>/dev/null; then \
			echo "  ✓ $$pkg"; \
		else \
			echo "  ✗ $$pkg - NOT FOUND"; \
		fi; \
	done
	@echo ""
	@echo "Checking SELinux..."
	@if [ "$(SELINUX_ENABLED)" = "yes" ]; then \
		echo "  ✓ SELinux is enabled"; \
	else \
		echo "  ✗ SELinux is disabled or not found"; \
	fi
	@echo ""
	@echo "Checking msgfmt..."
	@if [ "$(HAVE_MSGFMT)" = "yes" ]; then \
		echo "  ✓ msgfmt is available"; \
	else \
		echo "  ✗ msgfmt is not available (install gettext)"; \
	fi
	@echo ""
	@echo "Checking translations in locale directory..."
	@if [ -d locale ]; then \
		find locale -name "*.mo" 2>/dev/null | head -10 || echo "  No .mo files found"; \
	else \
		echo "  locale/ directory does not exist"; \
	fi
	@echo ""
	@echo "Checking source .po files..."
	@if [ -z "$(LANGUAGES)" ]; then \
		echo "  No languages configured"; \
	else \
		for lang in $(LANGUAGES); do \
			if [ -f "po/$$lang.po" ]; then \
				echo "  ✓ $$lang"; \
			else \
				echo "  ✗ $$lang - NOT FOUND"; \
			fi; \
		done; \
	fi

# ========== Помощь ==========
help:
	@echo "Available targets:"
	@echo "  all                  - Build plugin and translations"
	@echo "  install              - Install plugin and translations"
	@echo "  install-translations - Install only translations (from locale/ directory)"
	@echo "  uninstall            - Uninstall plugin"
	@echo "  clean                - Remove built files (including locale/ directory)"
	@echo "  debug                - Build with debug symbols"
	@echo "  translations         - Build translations (checks if already built)"
	@echo "  force-translations   - Force rebuild translations"
	@echo "  info                 - Show build information"
	@echo "  check                - Check dependencies"
	@echo "  help                 - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  LANGUAGES            - Space-separated list of languages (e.g. 'ru de fr')"
	@echo "  DESTDIR              - Installation prefix (e.g. DESTDIR=/tmp/test)"
	@echo ""
	@echo "Examples:"
	@echo "  make                           - Build with all languages"
	@echo "  make LANGUAGES=\"ru de\"         - Build with Russian and German only"
	@echo "  make LANGUAGES=\"\"              - Build without translations"
	@echo "  make force-translations        - Force rebuild all translations"
	@echo "  sudo make install              - Install plugin"
	@echo "  sudo make install-translations - Install translations from locale/ directory"

.PHONY: all install install-translations uninstall clean debug translations force-translations info check help
