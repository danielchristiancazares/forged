#!/bin/bash
. $(dirname $0)/common.inc

test_cflags -flto || skip

cat <<EOF | $CC -o $t/a.o -c -xc - -flto
#include <stdio.h>

void hello() {
  printf("Hello world\n");
}
EOF

cat <<EOF | $CC -o $t/b.o -c -xc - -flto
extern void missing(void);

__attribute__((constructor))
static void init(void) {
  missing();
}
EOF

ar rcs $t/c.a $t/a.o $t/b.o

cat <<EOF | $CC -o $t/d.o -c -xc -
void hello(void);

int main() {
  hello();
}
EOF

$CC --ld-path=./ld64 -o $t/exe $t/d.o $t/c.a -flto
$t/exe | grep -q 'Hello world'
