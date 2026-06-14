#!/bin/bash
#
# 测试结束后收集 SWOD 指标快照
# 只收集关键指标: swod_hold_cnt, swod_skip_cnt, undiscard_blks
# 用法: ./collect_swod_snapshot.sh
#

BASE=${BASE:-/sys/fs/ef2fs/nvme2n1p1}
STATUS=${STATUS:-/sys/kernel/debug/ef2fs/status}

echo "ts=$(date '+%F %T')"
echo "swod_hold_cnt=$(cat $BASE/swod_hold_cnt)"
echo "swod_skip_cnt=$(cat $BASE/swod_skip_cnt)"
echo "undiscard_blks=$(grep 'undiscard:' $STATUS | sed 's/.*undiscard://')"