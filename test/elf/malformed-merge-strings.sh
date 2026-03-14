#!/bin/bash
. $(dirname $0)/common.inc

cat <<'EOF' > $t/a.s
  .globl _start
  .text
_start:
  .section .rodata.bad,"aMS",@progbits,2
  .byte 0
EOF

$CC -c -o $t/a.o $t/a.s

! ./mold -o $t/exe $t/a.o 2> $t/log || false
grep -Fq 'string is not null terminated' $t/log
