CC      ?= clang
PKG     := gtk+-3.0
CFLAGS  += $(shell pkg-config --cflags $(PKG) 2>/dev/null)
LDFLAGS += $(shell pkg-config --libs $(PKG) 2>/dev/null)

TARGET  := simple-text-editor
SRC     := main.c
GLADE   := editor.glade

PREFIX  ?= /usr
BINDIR  := $(PREFIX)/bin
DATADIR := $(PREFIX)/share/$(TARGET)

# deb package
PKG_NAME    := simple-text-editor
PKG_VERSION := 1.0.0
PKG_ARCH    := $(shell dpkg-architecture -qDEB_HOST_ARCH 2>/dev/null || echo amd64)
DEB_DIR     := build/deb
DEB_ROOT    := $(DEB_DIR)/$(PKG_NAME)_$(PKG_VERSION)_$(PKG_ARCH)
DEB_FILE    := $(PKG_NAME)_$(PKG_VERSION)_$(PKG_ARCH).deb

.PHONY: help deps all clean run install deb

help:
	@echo "Usage:"
	@echo "  make deps   - check build dependencies"
	@echo "  make all    - build $(TARGET)"
	@echo "  make run    - build and run"
	@echo "  make deb    - build .deb package"
	@echo "  make clean  - remove binary and package artifacts"
	@echo "  make help   - show this help"

deps:
	@ok=1; \
	if ! command -v $(CC) >/dev/null 2>&1; then \
		echo "error: compiler '$(CC)' not found"; \
		echo "  install: sudo apt install clang"; \
		ok=0; \
	else \
		echo "ok: $(CC) ($$($(CC) --version | head -n1))"; \
	fi; \
	if ! command -v pkg-config >/dev/null 2>&1; then \
		echo "error: pkg-config not found"; \
		echo "  install: sudo apt install pkg-config"; \
		ok=0; \
	else \
		echo "ok: pkg-config"; \
	fi; \
	if ! command -v pkg-config >/dev/null 2>&1 || ! pkg-config --exists $(PKG); then \
		echo "error: $(PKG) not found"; \
		echo "  install: sudo apt install libgtk-3-dev"; \
		ok=0; \
	else \
		echo "ok: $(PKG) ($$(pkg-config --modversion $(PKG)))"; \
	fi; \
	if [ $$ok -eq 0 ]; then \
		echo ""; \
		echo "Fix missing dependencies and try again."; \
		exit 1; \
	fi; \
	echo "All dependencies OK"

all: deps $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

run: deps $(TARGET)
	./$(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -d $(DESTDIR)$(DATADIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -m 644 $(GLADE) $(DESTDIR)$(DATADIR)/$(GLADE)

deb: deps $(TARGET)
	@command -v dpkg-deb >/dev/null 2>&1 || { \
		echo "error: dpkg-deb not found"; \
		echo "  install: sudo apt install dpkg-dev"; \
		exit 1; \
	}
	rm -rf $(DEB_ROOT)
	$(MAKE) install DESTDIR=$(CURDIR)/$(DEB_ROOT) PREFIX=/usr
	install -d $(DEB_ROOT)/DEBIAN
	printf '%s\n' \
		'Package: $(PKG_NAME)' \
		'Version: $(PKG_VERSION)' \
		'Section: editors' \
		'Priority: optional' \
		'Architecture: $(PKG_ARCH)' \
		'Depends: libgtk-3-0' \
		'Maintainer: Simple Text Editor <noreply@localhost>' \
		'Description: Simple GTK3 text editor' \
		' A minimal text editor built with GTK3 and Glade.' \
		> $(DEB_ROOT)/DEBIAN/control
	fakeroot dpkg-deb --build $(DEB_ROOT) $(DEB_FILE)
	@echo "Built: $(DEB_FILE)"

clean:
	rm -f $(TARGET) $(DEB_FILE)
	rm -rf build
