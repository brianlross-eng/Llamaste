# Makefile for host-side update signing tools
# These compile with the HOST compiler (not Buildroot cross-compiler)

CC ?= gcc
CFLAGS = -O2 -Wall -I../src/llamaste
TWEETNACL = ../src/llamaste/tweetnacl.c

all: keygen sign-helper

keygen: keygen.c $(TWEETNACL)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

sign-helper: sign-helper.c $(TWEETNACL)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f keygen sign-helper keygen.exe sign-helper.exe

.PHONY: all clean
