#!/bin/bash
. $(dirname $0)/common.inc

cat <<'EOF' | $CC -o $t/a.o -c -x assembler -
.linker_option "-needed-l"
.text
.globl _main
_main:
  xorl %eax, %eax
  ret
EOF

$CC --ld-path=./ld64 -o $t/exe $t/a.o >& $t/log
grep -Fq 'ignoring malformed LC_LINKER_OPTION command: -needed-l' $t/log
$t/exe
