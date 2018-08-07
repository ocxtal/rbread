
CC = gcc
OFLAGS = -O3 -march=native
CFLAGS = $(OFLAGS) -std=c99 -Wall
LDFLAGS = -lz -lbz2 -llzma

all: scat.c zc.h
	$(CC) $(CFLAGS) -o scat scat.c $(LDFLAGS)

