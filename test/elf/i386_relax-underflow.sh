#!/bin/bash
. $(dirname $0)/common.inc

[ $MACHINE = i386 ] || skip
command -v python3 >& /dev/null || skip

cat <<'EOF' > $t/a.s
  .globl _start
  .text
_start:
  .long foo@GOT

  .data
  .globl foo
foo:
  .long 0
EOF

$CC -c -o $t/a.o $t/a.s

python3 - $t/a.o <<'PY'
import struct
import sys

path = sys.argv[1]
data = bytearray(open(path, 'rb').read())

ehdr = struct.Struct('<16sHHIIIIIHHHHHH')
shdr = struct.Struct('<IIIIIIIIII')
hdr = ehdr.unpack_from(data)
shoff = hdr[6]
shentsize = hdr[11]
shnum = hdr[12]

for i in range(shnum):
  sec = shdr.unpack_from(data, shoff + i * shentsize)
  if sec[1] != 9:
    continue

  info_off = sec[4] + 4
  info = struct.unpack_from('<I', data, info_off)[0]
  new_info = (info & ~0xff) | 43
  struct.pack_into('<I', data, info_off, new_info)

  with open(path, 'wb') as f:
    f.write(data)
  raise SystemExit(0)

raise SystemExit('missing REL section')
PY

./mold -m elf_i386 -o $t/exe $t/a.o
test -x $t/exe
