
CC = gcc
OFLAGS = -O3 -march=native
CFLAGS = $(OFLAGS) -std=c99 -Wall
LDFLAGS = -lz

all:
	$(CC) $(CFLAGS) -o zc zc.c $(LDFLAGS)

