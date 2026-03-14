#!/bin/bash
. $(dirname $0)/common.inc

cat <<'EOF' > $t/a.s
  .globl _start
  .text
_start:
  .section .eh_frame,"a",@progbits
  .byte 1
EOF

$CC -c -o $t/a.o $t/a.s

! ./mold -o $t/exe $t/a.o 2> $t/log || false
grep -Fq '.eh_frame' $t/log
grep -Fq 'corrupted .eh_frame' $t/log
