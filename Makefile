CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
CFLAGS  += $(shell pkg-config --cflags sdl3 libcurl)
LDLIBS  += $(shell pkg-config --libs sdl3 libcurl)

SRC = src/main.c src/video.c src/term.c src/keys.c
HDR = src/video.h src/term.h src/keys.h src/font8x8.h

c64uv: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

tests/run: tests/tests.c src/video.c src/term.c src/keys.c $(HDR)
	$(CC) $(CFLAGS) -o $@ tests/tests.c src/video.c src/term.c src/keys.c $(LDLIBS)

test: tests/run
	./tests/run

clean:
	rm -f c64uv tests/run

.PHONY: test clean
