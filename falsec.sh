#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2013  Jonathan Neuschäfer

if [ $# != 1 ]; then
	echo usage: falsec.sh SOURCECODE.f
	exit 1
fi

cat $1 | `dirname $0`/llfalse | opt -O2 > "$1.bc" || exit 1
llc --relocation-model=pic "$1.bc" && gcc -L . "$1.s" -lfalse -o "$1.bin"
