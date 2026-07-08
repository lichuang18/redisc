#!/bin/bash

OUT="backlog"
STATUS="/sys/kernel/debug/ef2fs/status"

if [ ! -r "$STATUS" ]; then
	echo "cannot read $STATUS" >&2
	exit 1
fi

printf "# date,discard_cmd_cnt,undiscard_blks\n" >> "$OUT"

trap 'echo "stopped, output: '$OUT'"; exit 0' INT TERM

while true; do
	now=$(date '+%Y-%m-%d %H:%M:%S')
	line=$(grep 'Discard:' "$STATUS")
	cmd=$(printf '%s\n' "$line" | sed -n 's/.*cmd:[[:space:]]*\([0-9][0-9]*\).*/\1/p')
	blks=$(printf '%s\n' "$line" | sed -n 's/.*undiscard:[[:space:]]*\([0-9][0-9]*\).*/\1/p')

	if [ -n "$cmd" ] && [ -n "$blks" ]; then
		printf "%s,%s,%s\n" "$now" "$cmd" "$blks" >> "$OUT"
	else
		printf "%s,NA,NA\n" "$now" >> "$OUT"
	fi

	sleep 1
done
