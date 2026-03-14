#!/bin/bash
. $(dirname $0)/common.inc

echo ' -help' > $t/rsp
./ld64 @$t/rsp | grep -q Usage

printf '\047\\' > $t/bad.rsp
if ./ld64 @$t/bad.rsp > /dev/null 2> $t/err; then
  false
fi
grep -q 'premature end of input' $t/err
