#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | $CC -c -o $t/a.o -xc -
void _start() {}
EOF

printf '\377\377\377\377\377\377\377\177' | \
  dd of=$t/a.o bs=1 seek=40 conv=notrunc status=none

! ./mold -m elf_x86_64 -r -o $t/b.o $t/a.o 2> $t/log || false
grep -Fq 'e_shoff or e_shnum corrupted' $t/log
