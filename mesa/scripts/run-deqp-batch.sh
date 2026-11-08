#!/bin/sh
# Several dEQP-GLES2.functional groups, JOBS at a time, each to build/deqp-out/<group>.{qpa,txt}.
# Usage: mesa/scripts/run-deqp-batch.sh JOBS group [group ...]    (a group name is the part after dEQP-GLES2.functional.)
# Then: mesa/scripts/deqp-summary.sh  for one line a group and the failing case names.
set -eu
cd "$(dirname "$0")/../.."
jobs=${1:?parallel jobs}; shift
printf '%s\n' "$@" | xargs -P "$jobs" -I{} sh -c 'mesa/scripts/run-deqp.sh "$1" "dEQP-GLES2.functional.$1.*" > /dev/null 2>&1; echo "done $1" >> build/deqp-out/batch.log' _ {}
