MESON ?= meson
BUILDDIR ?= build
MESON_ARGS ?=

.PHONY: all config build install uninstall clean distclean test

all: build

config:
	@if [ -d $(BUILDDIR)/meson-private ]; then \
		$(MESON) setup $(BUILDDIR) $(MESON_ARGS) --reconfigure; \
	else \
		$(MESON) setup $(BUILDDIR) $(MESON_ARGS); \
	fi

build:
	@if [ ! -d $(BUILDDIR)/meson-private ]; then \
		$(MESON) setup $(BUILDDIR) $(MESON_ARGS); \
	fi
	$(MESON) compile -C $(BUILDDIR)

install: build
	$(MESON) install -C $(BUILDDIR)

uninstall:
	$(MESON) uninstall -C $(BUILDDIR)

clean:
	@if [ -d $(BUILDDIR)/meson-private ]; then \
		$(MESON) compile -C $(BUILDDIR) --clean; \
	fi

distclean:
	rm -rf $(BUILDDIR)

test:
	$(MESON) test -C $(BUILDDIR)
