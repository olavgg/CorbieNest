CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
LDLIBS  += -lcjson -lcurl -pthread
PREFIX  ?= /usr/local

# The notcurses frontend (src/term_nc.c) is built when pkg-config finds notcurses (NOTCURSES=auto,
# the default), and left out otherwise: the build then needs nothing but libc, cJSON and libcurl.
# `make NOTCURSES=1` insists on it (fails without the library), `make NOTCURSES=0` leaves it out.
NOTCURSES ?= auto
ifeq ($(NOTCURSES),auto)
NOTCURSES := $(shell pkg-config --exists notcurses 2>/dev/null && echo 1 || echo 0)
endif
ifeq ($(NOTCURSES),1)
NC_CFLAGS ?= $(shell pkg-config --cflags notcurses)
NC_LIBS   ?= $(shell pkg-config --libs notcurses)
NC_DEFS   := -DWITH_NOTCURSES $(NC_CFLAGS)
LDLIBS    += $(NC_LIBS)
ifeq ($(strip $(NC_LIBS)),)
$(error NOTCURSES=1 but pkg-config does not find notcurses: install libnotcurses-dev, or set NC_CFLAGS/NC_LIBS)
endif
else
NC_DEFS   :=
endif

SRC := src/main.c src/util.c src/http.c src/term.c src/term_nc.c src/tools.c src/ollama.c src/provider.c src/skills.c
OBJ := $(SRC:.c=.o)

corbienest: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

# A stamp per setting: switching NOTCURSES rebuilds every object (their flags differ).
NC_STAMP := .notcurses-$(NOTCURSES)
$(NC_STAMP):
	rm -f .notcurses-*
	touch $@

src/%.o: src/%.c src/common.h $(NC_STAMP)
	$(CC) $(CFLAGS) $(NC_DEFS) -c -o $@ $<

# Optimized, stripped binary without debug info (a clean rebuild so no -g objects sneak in).
RELEASE_CFLAGS ?= -O2 -std=gnu11 -Wall -Wextra -DNDEBUG
release:
	$(MAKE) clean
	$(MAKE) corbienest CFLAGS="$(RELEASE_CFLAGS)" NOTCURSES=$(NOTCURSES)
	strip corbienest

install: corbienest
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 corbienest $(DESTDIR)$(PREFIX)/bin/corbienest

LIBOBJ := $(filter-out src/main.o,$(OBJ))

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
	rm -f $(OBJ) corbienest tests/test_unit .notcurses-*

.PHONY: install clean test test-unit test-integration release
