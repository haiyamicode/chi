#!/bin/sh
# Runs a test binary, optionally under valgrind (via $RUN), with clear reporting.
# Usage: run_test_bin.sh TEST_NAME STDOUT_FILE BIN [ARGS...]
# Captures stderr to a temp file so stdout-based .expect diffs stay clean.
# On failure, prints "error: TEST_NAME runtime failure (exit N)" and indents
# the captured stderr so valgrind's report is visible. On success, stays silent.
set -u
test_name=$1; shift
out=$1; shift
bin=$1; shift
err=$(mktemp)
${RUN:-} "$bin" "$@" >"$out" 2>"$err"
status=$?
if [ "$status" -ne 0 ]; then
    printf 'error: test %s runtime failure (exit %d)\n' "$test_name" "$status"
    sed 's/^/    /' "$err"
    rm -f "$err"
    exit "$status"
fi
rm -f "$err"
