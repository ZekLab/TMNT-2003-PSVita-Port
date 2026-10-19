#!/bin/bash
# Regenerate (with --gen), rebuild and run the Mac build for N seconds (default 60); log in run.log.
set -e
cd "$(dirname "$0")/.."
if [ "$1" = "--gen" ]; then
    shift
    .venv/bin/python recomp/gen_c.py
    .venv/bin/python recomp/gen_tables.py
fi
DEVELOPER_DIR=/Library/Developer/CommandLineTools make -j8 2>&1 | grep -E "error|warning: " | grep -v "^clang" || true
cd game/app
TMNT_TRACE_IMPORTS=${TRACE:-1} perl -e "alarm ${1:-60}; exec @ARGV" ../../build/mac/tmnt ../../recomp/out/image.bin > ../../run.log 2>&1 || echo "exit=$?"
grep -av "InitializeCriticalSection\|EnterCriticalSection\|LeaveCriticalSection" ../../run.log | tail -${TAIL:-25}
