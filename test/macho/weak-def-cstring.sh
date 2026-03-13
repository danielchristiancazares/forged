#!/bin/bash
. $(dirname $0)/common.inc

cat <<'EOF' | $CC -o $t/a.o -c -x assembler -
.subsections_via_symbols
.section __TEXT,__cstring,cstring_literals
.globl _msg
.weak_definition _msg
_msg:
  .asciz "hello"
EOF

cat <<'EOF' | $CC -o $t/b.o -c -x assembler -
.subsections_via_symbols
.section __TEXT,__cstring,cstring_literals
.globl _msg
_msg:
  .asciz "world"
EOF

cat <<'EOF' | $CC -o $t/c.o -c -xc -
extern const char msg[] asm("_msg");

int main() {
  return msg[0] != 'w';
}
EOF

$CC --ld-path=./ld64 -o $t/exe $t/a.o $t/b.o $t/c.o
$t/exe
