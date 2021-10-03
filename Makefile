# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2013  Jonathan Neuschäfer

all: llfalse libfalse.so falseflat

CC = gcc
#CFLAGS = -O2 -finline-functions -g
CFLAGS = -O0 -g
CFLAGS += -Wall -Wextra -Wwrite-strings -std=c99 -fPIC
#CFLAGS += -Werror
LDFLAGS += -g
LD = gcc
AR = ar

HAVE_LLVM:=$(shell llvm-config --version >/dev/null 2>&1 && echo 'yes')
LLVM_VERSION=$(shell llvm-config --version)

ifneq ($(HAVE_LLVM),yes)
$(error Your system doesn\'t have LLVM, but llfalse needs it to compile.)
else
endif

LLVM_COMPONENTS = core bitwriter analysis

LLVM_CFLAGS = $(shell llvm-config --cflags)
LLVM_LDFLAGS = $(shell llvm-config --ldflags --libs $(LLVM_COMPONENTS))
LLVM_LD = g++

# pretty printing
V             = @
Q             = $(V:1=)
QUIET_CC      = $(Q:@=@echo    '  CC  '$@;)
QUIET_LD      = $(Q:@=@echo    '  LD  '$@;)
QUIET_AR      = $(Q:@=@echo    '  AR  '$@;)

LLFALSE_OBJ=llfalse.o util.o

llfalse: $(LLFALSE_OBJ)
	$(QUIET_LD)$(LLVM_LD) $(LLFALSE_OBJ) $(LDFLAGS) $(LLVM_LDFLAGS) -o $@

llfalse.o: llfalse.c util.h
	$(QUIET_CC)$(CC) $(CFLAGS) $(LLVM_CFLAGS) -c $< -o $@

util.o: util.c
	$(QUIET_CC)$(CC) $(CFLAGS) -c $< -o $@

libfalse.so: libfalse.o
	$(QUIET_LD)$(LD) -shared $(LDFLAGS) $< -o $@

libfalse.o: libfalse.c
	$(QUIET_CC)$(CC) $(CFLAGS) -c $< -o $@

falseflat: falseflat.o
	$(QUIET_LD)$(LD) $< $(LDFLAGS) -o $@

falseflat.o: falseflat.c
	$(QUIET_CC)$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f llfalse libfalse.so falseflat *.o
