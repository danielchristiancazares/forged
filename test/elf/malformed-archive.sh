#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | $CC -c -o $t/a.o -xc -
void _start() {}
EOF

printf '!<arch>\n' > $t/bad.a
printf '%-16s%-12s%-6s%-6s%-8s%-10s`\n' '#1/32' 0 0 0 0 48 >> $t/bad.a
printf 'short-name' >> $t/bad.a

! ./mold -m elf_x86_64 -r -o $t/b.o $t/a.o $t/bad.a 2> $t/log || false
grep -Fq 'corrupted archive' $t/log
