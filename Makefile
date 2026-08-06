CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
CFLAGS  += $(shell pkg-config --cflags sdl3 libcurl)

# STATIC=1 links SDL3 and libcurl statically (glibc stays dynamic); used by
# the release workflow with a locally built SDL3/curl prefix on PKG_CONFIG_PATH.
ifdef STATIC
LDLIBS  += $(shell pkg-config --static --libs sdl3 libcurl)
LDFLAGS += -static-libgcc
else
LDLIBS  += $(shell pkg-config --libs sdl3 libcurl)
endif

SRC = src/main.c src/video.c src/term.c src/keys.c
HDR = src/video.h src/term.h src/keys.h src/font8x8.h

c64uv: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

tests/run: tests/tests.c src/video.c src/term.c src/keys.c $(HDR)
	$(CC) $(CFLAGS) -o $@ tests/tests.c src/video.c src/term.c src/keys.c $(LDLIBS)

test: tests/run
	./tests/run

clean:
	rm -f c64uv tests/run

.PHONY: test clean
