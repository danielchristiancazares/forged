#!/bin/bash
. $(dirname $0)/common.inc

[ $MACHINE = x86_64 ] || skip
command -v python3 >& /dev/null || skip

cat <<'EOF' > $t/a.s
  .globl _start
  .text
_start:
  .long foo@GOTPCREL

  .data
  .globl foo
foo:
  .quad 0
EOF

$CC -c -o $t/a.o $t/a.s

python3 - $t/a.o <<'PY'
import struct
import sys

path = sys.argv[1]
data = bytearray(open(path, 'rb').read())

ehdr = struct.Struct('<16sHHIQQQIHHHHHH')
shdr = struct.Struct('<IIQQQQIIQQ')
hdr = ehdr.unpack_from(data)
shoff = hdr[6]
shentsize = hdr[11]
shnum = hdr[12]

for i in range(shnum):
  sec = shdr.unpack_from(data, shoff + i * shentsize)
  if sec[1] != 4:
    continue

  info_off = sec[4] + 8
  info = struct.unpack_from('<Q', data, info_off)[0]
  new_info = (info & ~0xffffffff) | 42
  struct.pack_into('<Q', data, info_off, new_info)

  with open(path, 'wb') as f:
    f.write(data)
  raise SystemExit(0)

raise SystemExit('missing RELA section')
PY

./mold -o $t/exe $t/a.o
test -x $t/exe
