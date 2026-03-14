#!/bin/bash
. $(dirname $0)/common.inc

command -v python3 >& /dev/null || skip
echo 'int main() {}' | $CC -c -faddrsig -o /dev/null -xc - >& /dev/null || skip

cat <<'EOF' > $t/a.c
int foo;
int _start() {
  return foo;
}
EOF

$CC -c -faddrsig -o $t/a.o $t/a.c

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
shstrndx = hdr[13]

def get_shdr(i):
  return shdr.unpack_from(data, shoff + i * shentsize)

shstr = get_shdr(shstrndx)
shstr_off = shstr[4]

for i in range(shnum):
  sec = get_shdr(i)
  name_off = sec[0]
  name = bytes(data[shstr_off + name_off:]).split(b'\0', 1)[0]
  if name != b'.llvm_addrsig':
    continue

  off = sec[4]
  size = sec[5]
  if size == 0:
    raise SystemExit('empty .llvm_addrsig')

  data[off:off + size] = b'\x80' * size
  with open(path, 'wb') as f:
    f.write(data)
  raise SystemExit(0)

raise SystemExit('missing .llvm_addrsig')
PY

! ./mold --icf=safe -o $t/exe $t/a.o 2> $t/log || false
grep -Fq '.llvm_addrsig' $t/log
grep -Fq 'corrupted .llvm_addrsig' $t/log
