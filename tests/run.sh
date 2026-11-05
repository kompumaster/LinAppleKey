#!/bin/sh
# Replay tests. No root, no hidraw, no evdev, no uinput.
set -eu

unset JOURNAL_STREAM

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN=${BIN:-"$ROOT/build/linapplekeyd"}
CONF="$ROOT/tests/linapplekey.conf"
FIXTURES="$ROOT/tests/fixtures"
EXPECTED="$ROOT/tests/expected"

TMPDIR_TEST=$(mktemp -d "${TMPDIR:-/tmp}/linapplekey-test.XXXXXX")
trap 'rm -rf "$TMPDIR_TEST"' EXIT INT TERM

if [ ! -x "$BIN" ]; then
	echo "missing binary: $BIN (run make first)" >&2
	exit 1
fi

count=0
failed=0

for capture in "$FIXTURES"/*.txt; do
	[ -e "$capture" ] || continue

	name=$(basename "$capture" .txt)
	expected="$EXPECTED/$name.events"
	actual="$TMPDIR_TEST/$name.events"
	err="$TMPDIR_TEST/$name.err"

	if [ ! -f "$expected" ]; then
		echo "FAIL $name: missing $expected" >&2
		failed=1
		continue
	fi

	if ! "$BIN" --replay "$capture" -c "$CONF" >"$actual" 2>"$err"; then
		echo "FAIL $name: replay exited with error" >&2
		sed 's/^/  /' "$err" >&2
		failed=1
		continue
	fi

	if diff -u "$expected" "$actual"; then
		echo "ok   $name"
	else
		echo "FAIL $name: output differs" >&2
		failed=1
	fi

	count=$((count + 1))
done

if [ "$count" -eq 0 ]; then
	echo "FAIL: no fixtures in $FIXTURES" >&2
	exit 1
fi

# A broken config must produce warnings, keep working, and not change the
# replay result for options that were valid.
INVALID_CONF="$ROOT/tests/invalid.conf"
cfg_out="$TMPDIR_TEST/config.out"
cfg_err="$TMPDIR_TEST/config.err"

if ! "$BIN" --replay "$FIXTURES/a1314_plain.txt" -c "$INVALID_CONF" >"$cfg_out" 2>"$cfg_err"; then
	echo "FAIL config validation: replay exited with error" >&2
	sed 's/^/  /' "$cfg_err" >&2
	failed=1
else
	for pattern in "must be 0 or 1" "invalid value" "unknown key" "malformed line"; do
		if ! grep -q "$pattern" "$cfg_err"; then
			echo "FAIL config validation: missing warning '$pattern'" >&2
			failed=1
		fi
	done

	if ! diff -u "$EXPECTED/a1314_plain.events" "$cfg_out"; then
		echo "FAIL config validation: replay output changed" >&2
		failed=1
	fi

	for pattern in '[WARN] config'; do
		if ! grep -Fq "$pattern" "$cfg_err"; then
			echo "FAIL config validation: missing log level '$pattern'" >&2
			failed=1
		fi
	done

	if [ "$failed" -eq 0 ]; then
		echo "ok   config validation"
	fi
fi

log_err="$TMPDIR_TEST/log.err"
if ! "$BIN" --replay "$FIXTURES/a1314_plain.txt" -c "$CONF" >/dev/null 2>"$log_err"; then
	echo "FAIL log format: replay exited with error" >&2
	sed 's/^/  /' "$log_err" >&2
	failed=1
elif ! grep -Eq '^LinAppleKey [0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3} \[INFO\] replayed ' "$log_err"; then
	echo "FAIL log format: missing timestamp/level on replay summary" >&2
	sed 's/^/  /' "$log_err" >&2
	failed=1
else
	echo "ok   log format"
fi

journal_err="$TMPDIR_TEST/journal.err"
if ! JOURNAL_STREAM=8:1 "$BIN" --replay "$FIXTURES/a1314_plain.txt" -c "$CONF" >/dev/null 2>"$journal_err"; then
	echo "FAIL journald log format: replay exited with error" >&2
	sed 's/^/  /' "$journal_err" >&2
	failed=1
elif grep -Eq '^LinAppleKey [0-9]{4}-' "$journal_err"; then
	echo "FAIL journald log format: timestamp duplicated under journald" >&2
	sed 's/^/  /' "$journal_err" >&2
	failed=1
elif ! grep -Eq '^LinAppleKey \[INFO\] replayed ' "$journal_err"; then
	echo "FAIL journald log format: missing level" >&2
	sed 's/^/  /' "$journal_err" >&2
	failed=1
else
	echo "ok   journald log format"
fi

cli_err="$TMPDIR_TEST/cli.err"
if "$BIN" --duration nope >/dev/null 2>"$cli_err"; then
	echo "FAIL log level: invalid --duration accepted" >&2
	failed=1
elif ! grep -q '\[ERROR\] invalid --duration' "$cli_err"; then
	echo "FAIL log level: missing [ERROR] on CLI failure" >&2
	sed 's/^/  /' "$cli_err" >&2
	failed=1
fi

rate_cap="$TMPDIR_TEST/rate.cap"
{
	printf '# LinAppleKey capture v1\nMODEL A1314\n'
	i=0
	while [ "$i" -lt 3000 ]; do
		printf '0.000 R 9 01 00 00 00 00 00 00 00 00\n'
		i=$((i + 1))
	done
} >"$rate_cap"

rate_err="$TMPDIR_TEST/rate.err"
if ! "$BIN" --replay "$rate_cap" -v >/dev/null 2>"$rate_err"; then
	echo "FAIL debug rate limit: replay exited with error" >&2
	sed 's/^/  /' "$rate_err" >&2
	failed=1
else
	debug_lines=$(grep -c '\[DEBUG\]' "$rate_err" || true)
	if [ "$debug_lines" -lt 1 ] || [ "$debug_lines" -gt 1000 ]; then
		echo "FAIL debug rate limit: unexpected DEBUG line count $debug_lines" >&2
		failed=1
	elif ! grep -q 'debug rate limit: .* line(s) suppressed' "$rate_err"; then
		echo "FAIL debug rate limit: missing suppressed-line summary" >&2
		sed 's/^/  /' "$rate_err" >&2
		failed=1
	else
		echo "ok   debug rate limit"
	fi
fi

if command -v script >/dev/null 2>&1; then
	tty_cap="$TMPDIR_TEST/rate_tty.cap"
	{
		printf '# LinAppleKey capture v1\nMODEL A1314\n'
		i=0
		while [ "$i" -lt 200 ]; do
			printf '0.000 R 9 01 00 00 00 00 00 00 00 00\n'
			i=$((i + 1))
		done
	} >"$tty_cap"

	tty_log="$TMPDIR_TEST/rate_tty.log"
	if ! script -qec "$BIN --replay '$tty_cap' -v" "$tty_log" >/dev/null 2>&1; then
		echo "FAIL debug rate limit (tty): script failed" >&2
		failed=1
	else
		tty_debug=$(grep -c '\[DEBUG\]' "$tty_log" || true)
		if [ "$tty_debug" -lt 300 ] || grep -q 'debug rate limit:' "$tty_log"; then
			echo "FAIL debug rate limit (tty): expected unlimited DEBUG output" >&2
			failed=1
		else
			echo "ok   debug rate limit (tty)"
		fi
	fi
fi

doctor_out="$TMPDIR_TEST/doctor.out"
doctor_err="$TMPDIR_TEST/doctor.err"
if ! "$BIN" --doctor -c "$CONF" >"$doctor_out" 2>"$doctor_err"; then
	echo "FAIL doctor: exited with error" >&2
	sed 's/^/  /' "$doctor_err" >&2
	failed=1
else
	for pattern in "LinAppleKey doctor" "uinput:" "netlink:" "Supported Apple keyboards"; do
		if ! grep -q "$pattern" "$doctor_out"; then
			echo "FAIL doctor: missing '$pattern'" >&2
			failed=1
		fi
	done

	if [ "$failed" -eq 0 ]; then
		echo "ok   doctor"
	fi
fi

if [ "$failed" -ne 0 ]; then
	exit 1
fi

echo "all $count replay test(s) passed"
