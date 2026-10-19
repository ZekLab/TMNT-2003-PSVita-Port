#!/bin/bash
# Runs the Mac build of the translated game (sound with AUDIO=1); the log is run_user.log, older ones are kept in logs/.
cd "$(dirname "$0")/.."
mkdir -p logs
[ -f run_user.log ] && mv run_user.log "logs/run_user_$(date +%Y%m%d_%H%M%S).log"
cd game/app
TMNT_TRACE_IMPORTS=0 TMNT_AUDIO=${AUDIO:-} TMNT_AUDIO_DUMP=${AUDIO_DUMP:-} TMNT_TRACE_FUNCS=${TRACE_FUNCS:-} ../../build/mac/tmnt ../../recomp/out/image.bin > ../../run_user.log 2>&1 &
echo "TMNT pornit (pid $!)"
