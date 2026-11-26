#!/bin/sh
# Set the title ID in an XBE's certificate.
#   tools/xbe_titleid.sh <file.xbe> <8 hex digits>     e.g. 4D4D0058
#
# nxdk's cxbe always writes 0xFFFF0002, so the builds patch it afterwards.
# Header layout: base address at 0x104, certificate address at 0x118 (both
# virtual addresses; the headers are mapped at the base address), and the
# title ID is the dword at certificate + 8.
# SPDX-License-Identifier: MIT
set -e
xbe=$1
id=$2
case $id in
    [0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]) ;;
    *) echo "usage: $0 <file.xbe> <8 hex digits>" >&2; exit 1 ;;
esac
[ "$(dd if="$xbe" bs=1 count=4 2>/dev/null)" = XBEH ] || { echo "$xbe: not an XBE" >&2; exit 1; }

u32() { od -An -tu4 -j "$1" -N4 "$xbe" | tr -d ' \n'; }   # little-endian hosts
base=$(u32 260)        # 0x104
cert=$(u32 280)        # 0x118
off=$((cert - base + 8))

# Write the ID little-endian: bytes 7-8, 5-6, 3-4, 1-2 of the hex string.
b() { echo "$id" | cut -c"$1"; }
printf "\\$(printf %o 0x$(b 7-8))\\$(printf %o 0x$(b 5-6))\\$(printf %o 0x$(b 3-4))\\$(printf %o 0x$(b 1-2))" |
    dd of="$xbe" bs=1 seek="$off" conv=notrunc 2>/dev/null

echo "[ TITLEID  ] $xbe -> 0x$(echo "$id" | tr a-f A-F)"
