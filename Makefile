CC     ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -pedantic
PREFIX ?= /usr/local

all: eosfs

eosfs: eosfs.c
	$(CC) $(CFLAGS) -o $@ eosfs.c

install: eosfs
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 eosfs $(DESTDIR)$(PREFIX)/bin/eosfs

test: eosfs
	./test.sh

clean:
	rm -f eosfs

.PHONY: all install test clean
