#!/bin/sh
# Round-trip and layout tests for eosfs. Exits non-zero on any failure.
set -e

EOSFS=./eosfs
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "ok: $*"; }

# deterministic payloads of assorted sizes (block-boundary edge cases)
head -c 1        /dev/urandom > "$TMP/tiny"     # 1 byte      -> 1 blk, last=1
head -c 1024     /dev/urandom > "$TMP/exact"    # 1024 bytes  -> 1 blk, last=1024
head -c 1500     /dev/urandom > "$TMP/mid"      # 1500 bytes  -> 2 blk, last=476
head -c 4096     /dev/urandom > "$TMP/four"     # 4096 bytes  -> 4 blk (full group)
head -c 5000     /dev/urandom > "$TMP/big"      # 5000 bytes  -> 5 blk

roundtrip() {   # $1 = image, $2 = preset
    img="$TMP/$1"
    "$EOSFS" create "$img" "$2" -v TESTVOL >/dev/null
    for f in tiny exact mid four big; do
        "$EOSFS" add "$img" "$TMP/$f" --name "$f" >/dev/null
    done
    for f in tiny exact mid four big; do
        "$EOSFS" extract "$img" "$f" -o "$TMP/out_$f" >/dev/null
        cmp -s "$TMP/$f" "$TMP/out_$f" || fail "$1: $f data mismatch"
    done
    ok "$1: 5 files add + extract round-trip"

    # replace grows a file; others must remain intact
    "$EOSFS" replace "$img" "$TMP/big" --name mid >/dev/null
    "$EOSFS" extract "$img" mid -o "$TMP/out_mid" >/dev/null
    cmp -s "$TMP/big" "$TMP/out_mid" || fail "$1: replace mismatch"
    "$EOSFS" extract "$img" four -o "$TMP/out_four" >/dev/null
    cmp -s "$TMP/four" "$TMP/out_four" || fail "$1: neighbor corrupted by replace"
    ok "$1: replace preserves neighbours"

    # remove compacts; survivors must remain intact
    "$EOSFS" remove "$img" tiny >/dev/null
    "$EOSFS" remove "$img" exact >/dev/null
    for f in mid four big; do
        exp="$TMP/$f"; [ "$f" = mid ] && exp="$TMP/big"
        "$EOSFS" extract "$img" "$f" -o "$TMP/out_$f" >/dev/null
        cmp -s "$exp" "$TMP/out_$f" || fail "$1: $f corrupted after remove"
    done
    ok "$1: remove compacts without corruption"

    # image size is exactly blocks * 1024
    sz=$(wc -c < "$img")
    ok "$1: image size ${sz} bytes"
}

roundtrip test.ddp ddp256
roundtrip test.dsk dsk160

# custom sizes
"$EOSFS" create "$TMP/c.ddp" ddp -b 64 >/dev/null
"$EOSFS" create "$TMP/c.dsk" dsk -b 320 -d 5 >/dev/null
ok "custom ddp/dsk creation"

# --- boot blocks ---------------------------------------------------------
# default block 0 must be DI ; JP 0FCE7H  ->  f3 c3 e7 fc
"$EOSFS" create "$TMP/b.ddp" ddp256 >/dev/null
sig=$(dd if="$TMP/b.ddp" bs=1 count=4 status=none | od -An -tx1 | tr -d ' \n')
[ "$sig" = "f3c3e7fc" ] || fail "default boot is not JP SmartWriter (got $sig)"
ok "default boot block = JP SmartWriter"

# verbatim block install + preservation across a later add
head -c 1024 /dev/urandom > "$TMP/blk"
"$EOSFS" boot "$TMP/b.ddp" --block "$TMP/blk" >/dev/null
"$EOSFS" add  "$TMP/b.ddp" "$TMP/mid" --name keep >/dev/null
dd if="$TMP/b.ddp" bs=1 count=1024 status=none > "$TMP/blk_out"
cmp -s "$TMP/blk" "$TMP/blk_out" || fail "verbatim boot block not preserved"
ok "verbatim boot block install + preserved across add"

# BLOAD loader: header 01 00 02 <load> ; loader must skip 5 and read payload
printf '\001\000\002\000\020' > "$TMP/bload"      # load = 0x1000 (octal: portable to dash)
head -c 2043 /dev/urandom >> "$TMP/bload"          # 2048 total -> payload 2043
"$EOSFS" add  "$TMP/b.ddp" "$TMP/bload" --name PROGH >/dev/null
"$EOSFS" boot "$TMP/b.ddp" --file PROGH | grep -q "BLOAD, 2043 bytes) at 1000" \
    || fail "BLOAD loader did not report the header load address/length"
ok "BLOAD boot loader generated"

# raw loader: default load 0x0100, override respected
"$EOSFS" boot "$TMP/b.ddp" --file keep --load 0x2000 | grep -q "raw, .* at 2000" \
    || fail "raw loader --load override not honoured"
ok "raw boot loader + --load override"

echo "ALL TESTS PASSED"
