#!/bin/sh
# Build with MinGW-w64 (Linux or MSYS2)
set -e
VER=$(tr -d '[:space:]' < VERSION)
x86_64-w64-mingw32-windres res/app.rc -O coff -o res/app.res.o
x86_64-w64-mingw32-g++ -std=c++17 -O2 -DAPP_VERSION=\"$VER\" -municode -mwindows -static -s -o OLEDTextFix.exe src/main.cpp res/app.res.o \
  -ld3d11 -ldxgi -ldwmapi -lcomctl32 -lshell32 -luser32 -lgdi32 -ladvapi32 -lole32 -lwinhttp -lbcrypt
