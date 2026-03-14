#!/bin/bash
. $(dirname $0)/common.inc

cat <<'EOF' > $t/a.s
  .globl _start
  .text
_start:
  .section .debug_info,"",@progbits
  .long 0x1000
  .byte 4,0,0,0
EOF

$CC -c -o $t/a.o $t/a.s

! ./mold -gdb-index -o $t/exe $t/a.o 2> $t/log || false
grep -Fq '.debug_info' $t/log
grep -Fq 'corrupted .debug_info' $t/log
