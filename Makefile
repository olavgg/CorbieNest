CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
LDLIBS  += -lcjson -lcurl -pthread
PREFIX  ?= /usr/local

# The notcurses frontend (src/term_nc.c) and the notcurses core it runs on (vendor/notcurses, a copy of
# notcurses 3.0.17 with the tmux 3.4+ DA1 fix of PR #2926; see vendor/notcurses/README.md) are built
# when the libraries the core needs are there: libunistring (headers and library), zlib and terminfo
# (ncurses). NOTCURSES=auto (the default) builds them when they are and leaves the frontend out
# otherwise, so the build then needs nothing but libc, cJSON and libcurl. `make NOTCURSES=1` insists
# on them (fails without them), `make NOTCURSES=0` leaves them out.
NC_DIR   := vendor/notcurses
NC_LIBS  := -lunistring -ltinfo -lz -lm
NC_PROBE := printf '%s\n' '\#include <curses.h>' '\#include <term.h>' '\#include <zlib.h>' '\#include <unigbrk.h>' 'int main(void) { return 0; }' | $(CC) -x c - -o /dev/null $(NC_LIBS) >/dev/null 2>&1 && echo 1 || echo 0
NOTCURSES ?= auto
ifeq ($(NOTCURSES),auto)
NOTCURSES := $(shell $(NC_PROBE))
endif
ifeq ($(NOTCURSES),1)
ifneq ($(shell $(NC_PROBE)),1)
$(error NOTCURSES=1 but libunistring, zlib or terminfo is missing: install libunistring-dev zlib1g-dev libncurses-dev)
endif
NC_DEFS  := -DWITH_NOTCURSES -I$(NC_DIR)/include
LDLIBS   += $(NC_LIBS)
else
NC_DEFS  :=
endif

# The vendored notcurses core (built only with the frontend): its generated headers come from the
# templates CMake uses (tools/*.in), with the same substitutions, in build/notcurses/.
NC_VMAJ  := 3
NC_VMIN  := 0
NC_VPAT  := 17
NC_VTWK  := 0
NC_SRC   := $(wildcard $(NC_DIR)/src/lib/*.c) $(wildcard $(NC_DIR)/src/compat/*.c) $(NC_DIR)/src/media/shim.c $(NC_DIR)/src/media/none.c
NC_OBJ   := $(if $(filter 1,$(NOTCURSES)),$(NC_SRC:.c=.o))
NC_GEN   := build/notcurses/version.h build/notcurses/builddef.h
NC_VFLAGS := -D_XOPEN_SOURCE=700 -D_GNU_SOURCE -D_DEFAULT_SOURCE -I$(NC_DIR)/include -I$(NC_DIR)/src -I$(NC_DIR)/src/lib -I$(NC_DIR)/src/compat -Ibuild/notcurses -Wno-clobbered -Wno-discarded-qualifiers

SRC := src/main.c src/util.c src/http.c src/term.c src/term_nc.c src/tools.c src/ollama.c src/provider.c src/skills.c src/theme.c
OBJ := $(SRC:.c=.o)

corbienest: $(OBJ) $(NC_OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(NC_OBJ) $(LDLIBS)

# A stamp per setting: switching NOTCURSES rebuilds every object (their flags differ).
NC_STAMP := .notcurses-$(NOTCURSES)
$(NC_STAMP):
	rm -f .notcurses-*
	touch $@

src/%.o: src/%.c src/common.h $(NC_STAMP)
	$(CC) $(CFLAGS) $(NC_DEFS) -c -o $@ $<

# The vendored core'"'"'s own warnings are upstream'"'"'s: the two kinds it has (-Wclobbered in unixsig.c,
# -Wdiscarded-qualifiers in notcurses.c) are left out for it alone, so -Wall -Wextra -Werror still
# hold for our code.

build/notcurses/version.h: $(NC_DIR)/tools/version.h.in Makefile
	@mkdir -p build/notcurses
	sed -e 's/@notcurses_VERSION_MAJOR@/$(NC_VMAJ)/g' -e 's/@notcurses_VERSION_MINOR@/$(NC_VMIN)/g' -e 's/@notcurses_VERSION_PATCH@/$(NC_VPAT)/g' -e 's/@notcurses_VERSION_TWEAK@/$(NC_VTWK)/g' $< > $@

build/notcurses/builddef.h: $(NC_DIR)/tools/builddef.h.in Makefile
	@mkdir -p build/notcurses
	sed -e 's|@CMAKE_INSTALL_FULL_DATADIR@|$(PREFIX)/share|' -e '/cmakedefine/d' $< > $@

$(NC_DIR)/src/%.o: $(NC_DIR)/src/%.c $(NC_GEN)
	$(CC) $(CFLAGS) $(NC_VFLAGS) -c -o $@ $<

# Optimized, stripped binary without debug info (a clean rebuild so no -g objects sneak in).
RELEASE_CFLAGS ?= -O2 -std=gnu11 -Wall -Wextra -DNDEBUG
release:
	$(MAKE) clean
	$(MAKE) corbienest CFLAGS="$(RELEASE_CFLAGS)" NOTCURSES=$(NOTCURSES)
	strip corbienest

install: corbienest
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 corbienest $(DESTDIR)$(PREFIX)/bin/corbienest

LIBOBJ := $(filter-out src/main.o,$(OBJ)) $(NC_OBJ)

tests/test_unit: tests/test_unit.c $(LIBOBJ) src/common.h
	$(CC) $(CFLAGS) $(NC_DEFS) -o $@ tests/test_unit.c $(LIBOBJ) $(LDLIBS)

# `make test` is the unit tests: what a change has to pass, and what needs nothing but the build.
# The integration tests drive the program through a pty (and tmux), which is timing a hosted CI
# runner does not keep, and some checks reach the hosted APIs, which costs money: they run only
# on request, with `make test-integration`.
test: test-unit

test-unit: tests/test_unit
	./tests/test_unit

test-integration: corbienest
	python3 tests/test_integration.py

clean:
	rm -f $(OBJ) $(NC_SRC:.c=.o) corbienest tests/test_unit .notcurses-*
	rm -rf build

.PHONY: install clean test test-unit test-integration release
