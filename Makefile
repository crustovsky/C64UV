CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
CFLAGS  += $(shell pkg-config --cflags sdl3 libcurl)
LDLIBS  += $(shell pkg-config --libs sdl3 libcurl)

c64uv: src/main.c src/term.c src/term.h src/font8x8.h
	$(CC) $(CFLAGS) -o $@ src/main.c src/term.c $(LDLIBS)

clean:
	rm -f c64uv

.PHONY: clean
