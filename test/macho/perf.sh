#!/bin/bash
. $(dirname $0)/common.inc

cat <<EOF | $CC -o $t/a.o -c -xc -
int main() {}
EOF

$CC --ld-path=./ld64 -Wl,-perf -o $t/exe $t/a.o > $t/log 2>&1

grep -q 'compute_uuid$' $t/log
[ "$(grep -c 'copy_sections_to_output_file$' $t/log)" -eq 1 ]
