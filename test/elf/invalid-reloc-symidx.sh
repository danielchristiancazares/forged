#!/bin/bash
. $(dirname $0)/common.inc

command -v python3 >& /dev/null || skip

cat <<'EOF' > $t/a.s
  .globl _start
  .text
_start:
  .long foo
EOF

$CC -c -o $t/a.o $t/a.s

python3 - $t/a.o <<'PY'
import struct
import sys

path = sys.argv[1]
data = bytearray(open(path, 'rb').read())

ei_class = data[4]
ei_data = data[5]
endian = '<' if ei_data == 1 else '>'

if ei_class == 1:
  ehdr = struct.Struct(endian + '16sHHIIIIIHHHHHH')
  shdr = struct.Struct(endian + 'IIIIIIIIII')
elif ei_class == 2:
  ehdr = struct.Struct(endian + '16sHHIQQQIHHHHHH')
  shdr = struct.Struct(endian + 'IIQQQQIIQQ')
else:
  raise SystemExit('unsupported ELF class')

hdr = ehdr.unpack_from(data)
shoff = hdr[6]
shentsize = hdr[11]
shnum = hdr[12]

def get_shdr(i):
  return shdr.unpack_from(data, shoff + i * shentsize)

for i in range(shnum):
  sec = get_shdr(i)
  sh_type = sec[1]
  if sh_type not in (4, 9):
    continue

  off = sec[4]
  entsize = sec[9]
  if entsize == 0:
    continue

  if ei_class == 1:
    info_off = off + 4
    info = struct.unpack_from(endian + 'I', data, info_off)[0]
    new_info = (0xffffff << 8) | (info & 0xff)
    struct.pack_into(endian + 'I', data, info_off, new_info)
  else:
    info_off = off + 8
    info = struct.unpack_from(endian + 'Q', data, info_off)[0]
    new_info = (0xffffffff << 32) | (info & 0xffffffff)
    struct.pack_into(endian + 'Q', data, info_off, new_info)

  with open(path, 'wb') as f:
    f.write(data)
  raise SystemExit(0)

raise SystemExit('missing relocation section')
PY

! ./mold -r -o $t/b.o $t/a.o 2> $t/log || false
grep -Fq 'invalid relocation symbol index' $t/log
