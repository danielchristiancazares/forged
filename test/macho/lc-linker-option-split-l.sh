#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | $CC -o $t/libfoo.dylib -shared -xc -
#include <stdio.h>
void hello() {
  printf("Hello world\n");
}
EOF

cat <<'EOF' | $CC -o $t/a.o -c -x assembler -
.linker_option "-l", "foo"
.text
.globl _main
_main:
  pushq %rbp
  movq %rsp, %rbp
  call _hello
  xorl %eax, %eax
  popq %rbp
  ret
EOF

$CC --ld-path=./ld64 -o $t/exe $t/a.o -L$t -Wl,-dead_strip_dylibs
$t/exe > $t/log
grep -q 'Hello world' $t/log
otool -l $t/exe | grep -A3 LOAD_DY | grep -q libfoo.dylib
