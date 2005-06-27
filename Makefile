
macosx := true

CFLAGS += -O0 -Wall -g -c

CFLAGS += -I.

CC := gcc
LD := gcc

ifeq ($(strip $(macosx)),true)
  LDFLAGS += -framework Carbon -framework IOKit
endif

BASEOBJS := linux_evdev.o macosx_hidmanager.o manymouse.o

.PHONY: clean all

all: detect_mice test_manymouse_stdio test_manymouse_sdl

clean:
	rm -f *.o example/*.o test_manymouse_stdio test_manymouse_stdio detect_mice

%.o : %c
	$(CC) $(CFLAGS) -o $@ $+

example/test_manymouse_sdl.o : example/test_manymouse_sdl.c
	$(CC) $(CFLAGS) -o $@ $+ `sdl-config --cflags`

detect_mice: $(BASEOBJS) example/detect_mice.o
	$(LD) $(LDFLAGS) -o $@ $+

test_manymouse_stdio: $(BASEOBJS) example/test_manymouse_stdio.o
	$(LD) $(LDFLAGS) -o $@ $+

test_manymouse_sdl: $(BASEOBJS) example/test_manymouse_sdl.o
	$(LD) $(LDFLAGS) -o $@ $+ `sdl-config --libs`

# end of Makefile ...

