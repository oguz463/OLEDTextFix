#!/bin/sh
# Native tests (no Windows needed) plus a shader syntax check when glslangValidator is installed.
set -e
cd "$(dirname "$0")/.."
g++ -std=c++17 -O1 -Wall -Wextra -o /tmp/oledtextfix_test_core tests/test_core.cpp -lm
/tmp/oledtextfix_test_core
if command -v glslangValidator >/dev/null 2>&1; then
  python3 tests/check_shader.py
else
  echo "glslangValidator not found: skipping shader syntax check"
fi
