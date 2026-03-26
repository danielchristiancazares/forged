#!/bin/bash
. $(dirname $0)/common.inc

# libc re-exports libSystem; an explicit -lSystem must not add a second
# DylibFile with the same install name (dyld rejects duplicate LC_LOAD_DYLIB).

cat <<EOF | $CC -o $t/a.o -c -xc -
int main() { return 0; }
EOF

$CC --ld-path=./ld64 -o $t/exe $t/a.o -Wl,-lc -Wl,-lSystem

n=$(otool -l $t/exe | grep -c libSystem.B.dylib) || true
if [ "$n" -ne 1 ]; then
  echo "expected exactly one libSystem.B.dylib reference in load commands, got $n"
  otool -l $t/exe | grep -A3 LC_LOAD_DYLIB | grep libSystem || true
  exit 1
fi

$t/exe
