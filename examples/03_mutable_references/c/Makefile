# Detect platform
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    CC = clang
    LDFLAGS = -L../../.. -lmental
else
    CC = gcc
    LDFLAGS = -L../../.. -lmental
endif

CFLAGS = -I../../.. -std=c11

make:
	@$(CC) $(CFLAGS) 03_mutable_references.c -o 03_mutable_references $(LDFLAGS)
	@DYLD_LIBRARY_PATH=../../.. LD_LIBRARY_PATH=../../.. ./03_mutable_references
