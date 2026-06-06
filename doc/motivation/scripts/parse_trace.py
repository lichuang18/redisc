#!/usr/bin/env python3
# parse_trace.py - Parse trace-cmd output into queue.csv / issue.csv
# Usage: python3 parse_trace.py <trace_file> <out_dir>
#
# Input: trace-cmd report output (f2fs-discard.trace)
# Output:
#   queue.csv  - QUEUE events: t_ms, lstart, len, segno, win_id
#   issue.csv  - ISSUE events: t_ms, lstart, len, segno, win_id

import sys
import re
import csv
from pathlib import Path

# F2FS block size
BLKS_PER_SEG = 512
WIN_SEGS = 4
WIN_BLKS = WIN_SEGS * BLKS_PER_SEG  # 2048

def segno(lstart):
    return lstart // BLKS_PER_SEG

def win_id(lstart):
    return segno(lstart) // WIN_SEGS

def ts_to_ms(ts_str):
    """Convert trace timestamp string to ms (relative to first seen)."""
    # trace-cmd format: "    <idle>-0     [000] ..... 12345.678901234: ..."
    m = re.search(r'\d+\.(\d+)', ts_str)
    if m:
        ns_str = m.group(1)
        # pad to 9 digits
        ns_str = ns_str.ljust(9, '0')
        return int(ns_str) // 1_000_000
    return 0

def parse_f2fs_queue_discard(line):
    """Parse: f2fs_queue_discard: dev=(maj,min) blkstart=0xLLLL len=0xNNNN"""
    m = re.search(r'blkstart\s*=\s*0x([0-9a-fA-F]+)', line)
    n = re.search(r'blklen\s*=\s*0x([0-9a-fA-F]+)', line)
    if not m or not n:
        return None, None
    return int(m.group(1), 16), int(n.group(1), 16)

def parse_f2fs_issue_discard(line):
    """Parse: f2fs_issue_discard: dev=(maj,min) blkstart=0xLLLL len=0xNNNN"""
    return parse_f2fs_queue_discard(line)  # same format

def parse_trace(trace_path):
    queue_events = []
    issue_events = []

    abs_first_ts = None

    with open(trace_path, 'r') as f:
        for line in f:
            if 'f2fs_queue_discard' in line:
                ts_m = re.search(r'(\d+)\.(\d+)', line)
                if not ts_m:
                    continue
                sec = int(ts_m.group(1))
                ns_str = ts_m.group(2).ljust(9, '0')
                ts_ns = sec * 1_000_000_000 + int(ns_str)
                if abs_first_ts is None:
                    abs_first_ts = ts_ns
                t_ms = (ts_ns - abs_first_ts) // 1_000_000

                lstart, length = parse_f2fs_queue_discard(line)
                if lstart is None:
                    continue
                sn = segno(lstart)
                wid = win_id(lstart)
                queue_events.append((t_ms, lstart, length, sn, wid))

            elif 'f2fs_issue_discard' in line:
                ts_m = re.search(r'(\d+)\.(\d+)', line)
                if not ts_m:
                    continue
                sec = int(ts_m.group(1))
                ns_str = ts_m.group(2).ljust(9, '0')
                ts_ns = sec * 1_000_000_000 + int(ns_str)
                if abs_first_ts is None:
                    abs_first_ts = ts_ns
                t_ms = (ts_ns - abs_first_ts) // 1_000_000

                lstart, length = parse_f2fs_issue_discard(line)
                if lstart is None:
                    continue
                sn = segno(lstart)
                wid = win_id(lstart)
                issue_events.append((t_ms, lstart, length, sn, wid))

    return queue_events, issue_events

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <trace_file> <out_dir>")
        sys.exit(1)

    trace_path = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"[parse] reading {trace_path}")
    queue_events, issue_events = parse_trace(trace_path)
    print(f"[parse] queue={len(queue_events)} issue={len(issue_events)}")

    with open(out_dir / 'queue.csv', 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['t_ms', 'lstart', 'len', 'segno', 'win_id'])
        for ev in queue_events:
            w.writerow(ev)

    with open(out_dir / 'issue.csv', 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['t_ms', 'lstart', 'len', 'segno', 'win_id'])
        for ev in issue_events:
            w.writerow(ev)

    print(f"[parse] saved queue.csv and issue.csv to {out_dir}")

if __name__ == '__main__':
    main()