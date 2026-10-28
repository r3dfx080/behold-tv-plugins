#!/bin/sh
# Builds BPP_Waveform.dll (32-bit) with MinGW-w64 gcc -m32 and runs the self-test.
set -e
cd "$(dirname "$0")"
G=${GCC_BIN:-/c/Users/_redfox/gcc/bin}
API=../../PluginsSDK/PluginsSDK/C++/API
FLAGS="-m32 -O2 -Wall -I$API -static-libgcc -static-libstdc++"

"$G/g++" $FLAGS -DWAVEFORM_TEST -o waveform_test.exe BPP_Waveform.cpp -lgdi32
./waveform_test.exe

"$G/windres" -F pe-i386 -i BPP_Waveform.rc -o BPP_Waveform_res.o
"$G/g++" $FLAGS -shared -s -o BPP_Waveform.dll BPP_Waveform.cpp BPP_Waveform_res.o -lgdi32
"$G/objdump" -p BPP_Waveform.dll | sed -n '/Ordinal\/Name Pointer/,/^$/p'
