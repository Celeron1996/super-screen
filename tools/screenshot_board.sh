#!/bin/sh
# screenshot_board.sh — 主机侧抓取板端 fb0 并转 PNG
# See docs/plan-imx6ull.md §8.2

set -eu

OUT=${1:-fb.png}
BOARD_IP=${2:-192.168.1.14}
TMP=$(mktemp)

cleanup() {
    rm -f "$TMP"
}
trap cleanup EXIT INT TERM

ssh "root@${BOARD_IP}" 'dd if=/dev/fb0 bs=4096 count=600' > "$TMP" 2>/dev/null
convert -size 1024x600 -depth 8 bgra:"$TMP" "$OUT"
sha256sum "$OUT"
