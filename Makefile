CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
CFLAGS  += $(shell pkg-config --cflags sdl3 libcurl)
LDLIBS  += $(shell pkg-config --libs sdl3 libcurl)

c64uv: src/main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f c64uv

.PHONY: clean
