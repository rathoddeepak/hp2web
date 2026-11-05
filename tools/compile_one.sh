#!/bin/sh
# compile_one.sh <file.c> : one shard -> $OUT/<name>.o, through the content-hash cache $CACHE
f="$1"; o="$OUT/$(basename "$f" .c).o"
k="$CACHE/$( (echo "$FLAGSUM"; cat "$f") | shasum | cut -c1-24).o"
if [ -f "$k" ]; then cp "$k" "$o"; exit 0; fi
if $CC $CFLAGS -c -o "$o" "$f" 2>"$o.err"; then rm -f "$o.err"; cp "$o" "$k"; else echo "FAIL $f"; fi
