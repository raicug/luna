#!/bin/sh
# Builds the consumer with nothing but a compiler, the installed headers and the
# bundled library: no CMake, no Luau checkout. Usage: plain.sh <prefix> [cxx]
set -e
prefix=$1
cxx=${2:-c++}
here=$(dirname "$0")
"$cxx" -std=c++20 -I"$prefix/include" "$here/main.cpp" \
    "$prefix/lib/libluna_bundled.a" -o "$here/plain_consumer"
"$here/plain_consumer"
rm -f "$here/plain_consumer"
