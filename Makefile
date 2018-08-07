
CC = gcc
OFLAGS = -O3 -march=native
CFLAGS = $(OFLAGS) -std=c99 -Wall
LDFLAGS = -lz -lbz2

all: zc.c miniz.c miniz_tdef.c miniz_tinfl.c
	$(CC) $(CFLAGS) -o zc $^ $(LDFLAGS)

