CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
LDLIBS  += -lcjson
PREFIX  ?= /usr/local

SRC := src/main.c src/util.c src/http.c src/term.c src/tools.c src/ollama.c src/skills.c
OBJ := $(SRC:.c=.o)

corbienest: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c src/common.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Optimized, stripped binary without debug info (a clean rebuild so no -g objects sneak in).
RELEASE_CFLAGS ?= -O2 -std=gnu11 -Wall -Wextra -DNDEBUG
release:
	$(MAKE) clean
	$(MAKE) corbienest CFLAGS="$(RELEASE_CFLAGS)"
	strip corbienest

install: corbienest
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 corbienest $(DESTDIR)$(PREFIX)/bin/corbienest

LIBOBJ := $(filter-out src/main.o,$(OBJ))

tests/test_unit: tests/test_unit.c $(LIBOBJ) src/common.h
	$(CC) $(CFLAGS) -o $@ tests/test_unit.c $(LIBOBJ) $(LDLIBS)

test: corbienest tests/test_unit
	./tests/test_unit
	python3 tests/test_integration.py

clean:
	rm -f $(OBJ) corbienest tests/test_unit

.PHONY: install clean test release
