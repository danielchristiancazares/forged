#!/bin/bash
. $(dirname $0)/common.inc

cat <<'EOF' > $t/a.s
  .globl _start
  .text
_start:
  .section .debug_info,"",@progbits
  .long 7
  .short 4
  .long 0
  .byte 8

  .section .debug_gnu_pubnames,"",@progbits
  .long 32
  .short 2
  .long 0
  .long 11
  .long 1
  .byte 0x12
  .asciz "foo"
EOF

$CC -c -o $t/a.o $t/a.s

! ./mold -gdb-index -o $t/exe $t/a.o 2> $t/log || false
grep -Fq '.debug_gnu_pubnames' $t/log
grep -Fq 'corrupted' $t/log
