import struct, sys
d = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
nsec = struct.unpack_from('<H', d, pe + 6)[0]
optsz = struct.unpack_from('<H', d, pe + 20)[0]
ts = struct.unpack_from('<I', d, pe + 8)[0]
opt = pe + 24
ep = struct.unpack_from('<I', d, opt + 16)[0]
base = struct.unpack_from('<I', d, opt + 28)[0]
print('timestamp', ts, 'entry rva %#x base %#x' % (ep, base))
for i in range(nsec):
    o = opt + optsz + i * 40
    name = d[o:o+8].rstrip(b'\0').decode('latin1')
    vsz, va, rsz, rptr = struct.unpack_from('<IIII', d, o + 8)
    ch = struct.unpack_from('<I', d, o + 36)[0]
    print('%-8s va %#08x vsz %#08x raw %#08x flags %#x%s' % (name, va, vsz, rsz, ch, '  <- entry' if va <= ep < va + vsz else ''))
