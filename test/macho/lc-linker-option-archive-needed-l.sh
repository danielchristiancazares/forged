#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | $CC -o $t/libfoo.dylib -shared -xc -
#include <stdio.h>
void foo() {
  printf("Hello world\n");
}
EOF

cat <<'EOF' | $CC -o $t/a.o -c -x assembler -
.linker_option "-needed-lfoo"
.text
.globl _archive_hello
_archive_hello:
  ret
EOF

rm -f $t/liba.a
ar rcs $t/liba.a $t/a.o

cat <<EOF | $CC -o $t/b.o -c -xc -
void archive_hello();

int main() {
  archive_hello();
}
EOF

$CC --ld-path=./ld64 -o $t/exe $t/b.o $t/liba.a -L$t -Wl,-dead_strip_dylibs
otool -l $t/exe | grep -A3 LOAD_DY | grep -q libfoo.dylib
