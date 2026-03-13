#!/bin/bash
. $(dirname $0)/common.inc

cat <<'EOF' | $CC -o $t/a.o -c -x assembler -
.section __TEXT,__cstring,cstring_literals
.private_extern _msg
_msg:
  .asciz "hello"
EOF

nm -m $t/a.o | grep -q '(__TEXT,__cstring) private external _msg'

cat <<'EOF' | $CC -o $t/b.o -c -x assembler -
.text
.globl _main
_main:
  leaq _msg(%rip), %rax
  movzbl (%rax), %eax
  cmpl $104, %eax
  sete %al
  movzbl %al, %eax
  xorl $1, %eax
  ret
EOF

ar rcs $t/liba.a $t/a.o
$CC --ld-path=./ld64 -o $t/exe $t/b.o $t/liba.a
$t/exe
