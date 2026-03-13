#!/bin/bash
. $(dirname $0)/common.inc

cat <<'EOF' | $CC -o $t/a.o -c -x assembler -
.section __DATA,__foo
.globl _x
_x:
  .quad 0
EOF

cat <<'EOF' | $CC -o $t/b.o -c -xc -
extern long x asm("_x");

int main() {
  return x != 0;
}
EOF

$CC --ld-path=./ld64 -o $t/exe $t/a.o $t/b.o \
  -Wl,-sectalign,__DATA,__foo,0x1000

otool -l $t/exe | grep -A8 'sectname __foo' | grep -q 'align 2\^12 (4096)'

$t/exe
