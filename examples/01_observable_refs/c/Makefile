# Detect platform
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    CC = clang
    LDFLAGS = -L../../.. -lmental -lpthread
else
    CC = gcc
    LDFLAGS = -L../../.. -lmental -lpthread
endif

CFLAGS = -I../../.. -std=c11

make:
	@$(CC) $(CFLAGS) 01_observable_refs.c -o 01_observable_refs $(LDFLAGS)
	@DYLD_LIBRARY_PATH=../../.. LD_LIBRARY_PATH=../../.. ./01_observable_refs
