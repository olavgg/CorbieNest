CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
LDLIBS  += -lcjson -lcurl -pthread
PREFIX  ?= /usr/local

# SAN=address, undefined or thread builds with that sanitizer (AddressSanitizer, UndefinedBehaviorSanitizer,
# ThreadSanitizer). Every object is built with it, and the sanitizer's reports are fatal (-fno-sanitize-recover).
# A stamp per setting rebuilds the objects when SAN changes, as for NOTCURSES. See test-asan, test-ubsan, test-tsan.
SAN ?=
ifneq ($(SAN),)
CFLAGS += -fsanitize=$(SAN) -fno-sanitize-recover=all -fno-omit-frame-pointer
endif
SAN_STAMP := .sanitize-$(or $(SAN),none)

# Homebrew's cJSON sits outside the compiler's default search path. These stay
# out of CFLAGS, so a command-line CFLAGS= (CI, `make release`) still finds it.
ifeq ($(shell uname -s),Darwin)
  BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
  ifeq ($(BREW_PREFIX),)
    BREW_PREFIX := $(shell if [ -f /opt/homebrew/include/cjson/cJSON.h ]; then echo /opt/homebrew; elif [ -f /usr/local/include/cjson/cJSON.h ]; then echo /usr/local; fi)
  endif
  ifneq ($(BREW_PREFIX),)
    BREW_CPPFLAGS := -I$(BREW_PREFIX)/include
    BREW_LDFLAGS  := -L$(BREW_PREFIX)/lib
  endif
endif

# The notcurses frontend (src/term_nc.c) and the notcurses core it runs on (vendor/notcurses, a copy of
# notcurses 3.0.17 with the tmux 3.4+ DA1 fix of PR #2926; see vendor/notcurses/README.md) are built
# when the libraries the core needs are there: libunistring (headers and library), zlib and terminfo
# (ncurses). NOTCURSES=auto (the default) builds them when they are and leaves the frontend out
# otherwise, so the build then needs nothing but libc, cJSON and libcurl. `make NOTCURSES=1` insists
# on them (fails without them), `make NOTCURSES=0` leaves them out.
NC_DIR   := vendor/notcurses
# terminfo is libtinfo on Linux; macOS has no libtinfo, its terminfo is in libncurses
NC_TINFO := $(if $(filter Darwin,$(shell uname -s)),-lncurses,-ltinfo)
NC_LIBS  := -lunistring $(NC_TINFO) -lz -lm
NC_PROBE := printf '%s\n' '\#include <curses.h>' '\#include <term.h>' '\#include <zlib.h>' '\#include <unigbrk.h>' 'int main(void) { return 0; }' | $(CC) $(BREW_CPPFLAGS) -x c - -o /dev/null $(BREW_LDFLAGS) $(NC_LIBS) >/dev/null 2>&1 && echo 1 || echo 0
# zlib is optional too: gzip request bodies (util.c gzip_body, WITH_ZLIB) need it. ZLIB=auto (the default) builds
# with it when zlib.h and -lz are there; ZLIB=0 leaves it out; ZLIB=1 insists on it.
ZLIB_PROBE := printf '%s\n' '\#include <zlib.h>' 'int main(void) { return 0; }' | $(CC) $(BREW_CPPFLAGS) -x c - -o /dev/null $(BREW_LDFLAGS) -lz >/dev/null 2>&1 && echo 1 || echo 0
ZLIB ?= auto
ifeq ($(ZLIB),auto)
ZLIB := $(shell $(ZLIB_PROBE))
endif
ifeq ($(ZLIB),1)
ifneq ($(shell $(ZLIB_PROBE)),1)
$(error ZLIB=1 but zlib.h or -lz is missing: install zlib1g-dev)
endif
ZLIB_DEFS := -DWITH_ZLIB
else
ZLIB_DEFS :=
endif
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
# -lz once: the notcurses libraries already carry it
ifeq ($(ZLIB),1)
ifneq ($(NOTCURSES),1)
LDLIBS += -lz
endif
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
# _DARWIN_C_SOURCE is what exposes strcasecmp and friends on macOS (ignored elsewhere); the two -Wno- for
# clang's names of the upstream GCC warnings (gcc ignores the unknown ones)
NC_VFLAGS := -D_XOPEN_SOURCE=700 -D_GNU_SOURCE -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -I$(NC_DIR)/include -I$(NC_DIR)/src -I$(NC_DIR)/src/lib -I$(NC_DIR)/src/compat -Ibuild/notcurses -Wno-clobbered -Wno-discarded-qualifiers -Wno-unknown-warning-option -Wno-incompatible-pointer-types-discards-qualifiers

SRC := src/main.c src/util.c src/http.c src/term.c src/term_nc.c src/tools.c src/ollama.c src/provider.c src/skills.c src/theme.c src/mcp.c src/agents.c src/stream.c
OBJ := $(SRC:.c=.o)

corbienest: $(OBJ) $(NC_OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(NC_OBJ) $(BREW_LDFLAGS) $(LDLIBS)

# A stamp per setting: switching NOTCURSES rebuilds every object (their flags differ).
NC_STAMP := .notcurses-$(NOTCURSES)
$(NC_STAMP):
	rm -f .notcurses-*
	touch $@

$(SAN_STAMP):
	rm -f .sanitize-*
	touch $@

# A stamp per setting, as for NOTCURSES: switching ZLIB rebuilds the objects that read WITH_ZLIB.
ZLIB_STAMP := .zlib-$(ZLIB)
$(ZLIB_STAMP):
	rm -f .zlib-*
	touch $@

src/%.o: src/%.c src/common.h $(NC_STAMP) $(SAN_STAMP) $(ZLIB_STAMP)
	$(CC) $(BREW_CPPFLAGS) $(CFLAGS) $(NC_DEFS) $(ZLIB_DEFS) $(SKILL_DEFS) -c -o $@ $<

# The vendored core'"'"'s own warnings are upstream'"'"'s: the two kinds it has (-Wclobbered in unixsig.c,
# -Wdiscarded-qualifiers in notcurses.c) are left out for it alone, so -Wall -Wextra -Werror still
# hold for our code.

build/notcurses/version.h: $(NC_DIR)/tools/version.h.in Makefile
	@mkdir -p build/notcurses
	sed -e 's/@notcurses_VERSION_MAJOR@/$(NC_VMAJ)/g' -e 's/@notcurses_VERSION_MINOR@/$(NC_VMIN)/g' -e 's/@notcurses_VERSION_PATCH@/$(NC_VPAT)/g' -e 's/@notcurses_VERSION_TWEAK@/$(NC_VTWK)/g' $< > $@

build/notcurses/builddef.h: $(NC_DIR)/tools/builddef.h.in Makefile
	@mkdir -p build/notcurses
	sed -e 's|@CMAKE_INSTALL_FULL_DATADIR@|$(PREFIX)/share|' -e '/cmakedefine/d' $< > $@

$(NC_DIR)/src/%.o: $(NC_DIR)/src/%.c $(NC_GEN) $(SAN_STAMP)
	$(CC) $(BREW_CPPFLAGS) $(CFLAGS) $(NC_VFLAGS) -c -o $@ $<

# Optimized, stripped binary without debug info (a clean rebuild so no -g objects sneak in).
RELEASE_CFLAGS ?= -O2 -std=gnu11 -Wall -Wextra -DNDEBUG
release:
	$(MAKE) clean
	$(MAKE) corbienest CFLAGS="$(RELEASE_CFLAGS)" NOTCURSES=$(NOTCURSES) ZLIB=$(ZLIB)
	strip corbienest

# The skills that ship with corbienest (skills/, read-only, source "bundled"): make install copies them to
# $(PREFIX)/share/corbienest/skills, and the build also names the checkout's own skills/, so a run from the
# checkout has them without installing. The paths go into skills.o only; a stamp rebuilds it when they change.
BUNDLED_SKILLS_DIR := $(PREFIX)/share/corbienest/skills
SKILLS_STAMP := .skills-$(shell printf '%s' '$(BUNDLED_SKILLS_DIR)$(CURDIR)' | cksum | cut -d' ' -f1)
$(SKILLS_STAMP):
	rm -f .skills-*
	touch $@

src/skills.o: SKILL_DEFS := -DBUNDLED_SKILLS_DIR='"$(BUNDLED_SKILLS_DIR)"' -DBUNDLED_SKILLS_SRC='"$(CURDIR)/skills"'
src/skills.o: $(SKILLS_STAMP)

install: corbienest
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 corbienest $(DESTDIR)$(PREFIX)/bin/corbienest
	rm -rf $(DESTDIR)$(BUNDLED_SKILLS_DIR)
	install -d $(DESTDIR)$(BUNDLED_SKILLS_DIR)
	cp -R skills/. $(DESTDIR)$(BUNDLED_SKILLS_DIR)/

LIBOBJ := $(filter-out src/main.o,$(OBJ)) $(NC_OBJ)

tests/test_unit: tests/test_unit.c $(LIBOBJ) src/common.h
	$(CC) $(BREW_CPPFLAGS) $(CFLAGS) $(NC_DEFS) $(ZLIB_DEFS) -o $@ tests/test_unit.c $(LIBOBJ) $(BREW_LDFLAGS) $(LDLIBS)

# `make test` is the unit tests: what a change has to pass, and what needs nothing but the build.
# The integration tests drive the program through a pty (and tmux), which is timing a hosted CI
# runner does not keep, and some checks reach the hosted APIs, which costs money: they run only
# on request, with `make test-integration`.
test: test-unit

test-unit: tests/test_unit
	./tests/test_unit

test-integration: corbienest
	python3 tests/test_integration.py

# The unit suite under each sanitizer: AddressSanitizer (memory errors; leaks where the platform reports them),
# UndefinedBehaviorSanitizer (undefined behaviour, fatal), ThreadSanitizer (data races: the http job threads and
# the notcurses input thread). Each one builds its own objects, so the next plain `make` builds them again.
# On Apple's toolchain as of clang 17 / macOS 26, test-asan hangs at start-up and test-tsan segfaults even for a
# trivial program; test-ubsan runs there. The other two need a Linux machine.
test-asan:
	$(MAKE) SAN=address test-unit

test-ubsan:
	$(MAKE) SAN=undefined test-unit

test-tsan:
	$(MAKE) SAN=thread test-unit

clean:
	rm -f $(OBJ) $(NC_SRC:.c=.o) corbienest tests/test_unit .notcurses-* .sanitize-* .zlib-* .skills-*
	rm -rf build

.PHONY: install clean test test-unit test-integration test-asan test-ubsan test-tsan release
