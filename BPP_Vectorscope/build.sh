#!/bin/sh
# Builds BPP_Vectorscope.dll (32-bit) with MinGW-w64 gcc -m32 and runs the self-test.
# The self-test takes an optional .bmp path and renders 75 % bars onto the scope there.
set -e
cd "$(dirname "$0")"
G=${GCC_BIN:-/c/Users/_redfox/gcc/bin}
API=../../PluginsSDK/PluginsSDK/C++/API
FLAGS="-m32 -O2 -Wall -I$API -static-libgcc -static-libstdc++"

"$G/g++" $FLAGS -DVECTORSCOPE_TEST -o vectorscope_test.exe BPP_Vectorscope.cpp -lgdi32 -lcomctl32
./vectorscope_test.exe

"$G/windres" -F pe-i386 -i BPP_Vectorscope.rc -o BPP_Vectorscope_res.o
"$G/g++" $FLAGS -shared -s -o BPP_Vectorscope.dll BPP_Vectorscope.cpp BPP_Vectorscope_res.o -lgdi32 -lcomctl32
"$G/objdump" -p BPP_Vectorscope.dll | sed -n '/Ordinal\/Name Pointer/,/^$/p'
