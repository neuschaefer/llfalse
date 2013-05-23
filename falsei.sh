#!/bin/sh
# falsei.sh - a simple llfalse based False interpreter
# usage: falsei.sh FILE.F

dir=$(dirname "$0")

tmpfile=$(mktemp)

$dir/llfalse < $1 opt -O2 > $tmpfile
lli -load=$dir/libfalse.so $tmpfile
rm $tmpfile
