#!/bin/bash
#
# Runs clang format over the whole project tree, excluding the 'build/' directory.
#

find . -type d \( -path './build' \) -prune -iname '*.h' -o -iname '*.c' | xargs clang-format -i

