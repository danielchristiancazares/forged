#!/bin/bash
. $(dirname $0)/common.inc

mkdir -p $t/Foo.framework

cat <<EOF | $CC -o $t/Foo.framework/Foo -shared -xc -
void foo() {}
EOF

cat <<'EOF' | $CC -o $t/a.o -c -x assembler -
.linker_option "-needed-framework", "Foo"
.text
.globl _main
_main:
  xorl %eax, %eax
  ret
EOF

$CC --ld-path=./ld64 -o $t/exe $t/a.o -Wl,-F$t -Wl,-framework,Foo \
  -Wl,-dead_strip_dylibs
otool -l $t/exe | grep -A3 'cmd LC_LOAD_DYLIB' | grep -Fq Foo.framework/Foo
