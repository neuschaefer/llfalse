#!/bin/sh
if [ $# != 1 ]; then
	echo usage: falsec.sh SOURCECODE.f
	exit 1
fi

cat $1 | `dirname $0`/llfalse | opt -O2 > "$1.bc" || exit 1
llc "$1.bc" && gcc -L . -lfalse "$1.s" -o "$1.bin"
