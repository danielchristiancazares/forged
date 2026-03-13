#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | $CC -o $t/a.o -c -xc -
void foo() {}
EOF

$CC --ld-path=./ld64 -shared -o $t/libfoo.dylib $t/a.o

cat <<EOF | $CC -o $t/b.o -c -xc -
int main() { return 0; }
EOF

! $CC --ld-path=./ld64 -o $t/exe $t/b.o $t/libfoo.dylib \
  -Wl,-application_extension -Wl,-fatal_warnings >& $t/log

grep -q 'not safe for use in application extensions' $t/log
