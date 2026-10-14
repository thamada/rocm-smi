CC      ?= gcc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -D_GNU_SOURCE
ROCM    ?= /opt/rocm
CPPFLAGS ?= -I$(ROCM)/include
LDFLAGS ?= -L$(ROCM)/lib -Wl,-rpath,$(ROCM)/lib
LIBS    ?= -lamd_smi

.PHONY: all build run clean

all: build

build: rocm-smi

rocm-smi: rocm-smi.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $< $(LDFLAGS) $(LIBS)

run: build
	./rocm-smi

clean:
	rm -f rocm-smi
