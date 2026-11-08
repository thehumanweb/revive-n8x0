#!/bin/sh
# One line a group (pass/fail/not supported) from build/deqp-out/*.txt, then the failing cases with their reason.
cd "$(dirname "$0")/../.."
for f in build/deqp-out/*.txt; do
	n=$(basename $f .txt); [ "$n" = batch ] && continue
	p=$(grep -c '^  Pass' $f); fl=$(grep -c '^  Fail' $f); ns=$(grep -c '^  NotSupported' $f); o=$(grep -c -E '^  (Crash|Timeout|InternalError)' $f)
	echo "$n: $p pass, $fl fail, $ns not supported, $o crash/timeout/internal error$(grep -q 'DONE!' $f || echo ' (not finished)')"
done
echo; awk '/^Test case/{n=$3} /^  (Fail|Crash|Timeout|InternalError)/{sub(/^  /,""); print n, $0}' build/deqp-out/*.txt | cut -c1-210
