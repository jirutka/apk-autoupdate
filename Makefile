PACKAGE       := apk-autoupdate
VERSION       := $(shell cat VERSION)

prefix        := /usr/local
sbindir       := $(prefix)/sbin
datadir       := $(prefix)/share/$(PACKAGE)
sysconfdir    := /etc

BUILD_DIR     := build
BIN_FILES     := apk-autoupdate procs-need-restart rc-service-pid
DATA_FILES    := functions.sh openrc.sh

INSTALL       := install
SED           := sed

GIT_REV		  := $(shell test -d .git && git describe 2>/dev/null || echo exported)
ifneq ($(GIT_REV), exported)
  VERSION     := $(patsubst $(PACKAGE)-%,%,$(GIT_REV))
  VERSION     := $(patsubst v%,%,$(VERSION))
endif

ifeq ($(DEBUG), 1)
  CFLAGS      := -g -DDEBUG
  CFLAGS      += -Wall -Wextra -pedantic
  ifeq ($(shell $(CC) --version | grep -q clang && echo clang), clang)
    CFLAGS    += -Weverything -Wno-vla
  endif
else
  CFLAGS      ?= -Os -DNDEBUG
endif

D              = $(BUILD_DIR)
MAKEFILE_PATH  = $(lastword $(MAKEFILE_LIST))
VPATH          = src


all: build

#: Build sources (default target).
build: $(addprefix $(D)/,$(BIN_FILES))

#: Remove generated files.
clean:
	rm -Rf "$(D)"

#: Install into $DESTDIR.
install: install-conf install-data install-exec

#: Install configuration files into $DESTDIR/$sysconfdir/apk/.
install-conf: etc/autoupdate.conf
	$(INSTALL) -d $(DESTDIR)$(sysconfdir)/apk/
	$(INSTALL) -m 640 $< $(DESTDIR)$(sysconfdir)/apk/

#: Install data files into $DESTDIR/$datadir/.
install-data: $(DATA_FILES)
	$(INSTALL) -d $(DESTDIR)$(datadir)
	for file in $(DATA_FILES); do \
		$(INSTALL) -m 644 src/$$file $(DESTDIR)$(datadir)/$$file; \
	done

#: Install executables into $DESTDIR/$sbindir/.
install-exec: build
	$(INSTALL) -d $(DESTDIR)$(sbindir)
	for file in $(BIN_FILES); do \
		$(INSTALL) -m 755 $(D)/$$file $(DESTDIR)$(sbindir)/$$file; \
	done

#: Print list of targets.
help:
	@printf '%s\n\n' 'List of targets:'
	@$(SED) -En '/^#:.*/{ N; s/^#: (.*)\n([A-Za-z0-9_-]+).*/\2 \1/p }' $(MAKEFILE_PATH) \
		| while read label desc; do printf '%-30s %s\n' "$$label" "$$desc"; done

.PHONY: all build clean install install-conf install-data install-exec help


$(D)/%: %.in | .builddir
	$(SED) -e 's|@VERSION@|$(VERSION)|g' \
	       -e 's|@datadir@|$(datadir)|g' \
	       -e 's|@sysconfdir@|$(sysconfdir)|g' \
	       $< > $@
	chmod +x $@

$(D)/%.o: %.c | .builddir
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -DVERSION=$(VERSION) -o $@ -c $<

$(D)/%: $(D)/%.o
	$(CC) $(LDFLAGS) -o $@ $<

.builddir:
	@mkdir -p "$(D)"

.PHONY: .builddir
