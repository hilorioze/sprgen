CC = gcc
CFLAGS = -Wall -O2 -std=c99
LDFLAGS = -lm

all: sprgen sprinfo

sprgen: sprgen.c
	$(CC) $(CFLAGS) -o sprgen sprgen.c $(LDFLAGS)

sprinfo: sprinfo.c
	$(CC) $(CFLAGS) -o sprinfo sprinfo.c

clean:
	rm -f sprgen sprinfo

debug: CFLAGS += -g -O0
debug: all

.PHONY: all clean debug
