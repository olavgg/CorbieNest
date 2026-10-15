# The screenshot helpers (see SKILL.md). Not part of the corbienest build.
CC ?= cc
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra
# forkpty is in libutil on glibc older than 2.34; the BSDs and macOS have it in libc
LDLIBS ?= $(if $(filter Linux,$(shell uname -s)),-lutil)

all: capture screen

capture: capture.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

screen: screen.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f capture screen

.PHONY: all clean
