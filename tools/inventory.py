import sys, collections, re
import pefile, capstone
pe = pefile.PE(sys.argv[1])
base = pe.OPTIONAL_HEADER.ImageBase
print('== imports')
for e in pe.DIRECTORY_ENTRY_IMPORT:
    names = [i.name.decode() if i.name else 'ord%d' % i.ordinal for i in e.imports]
    print('%s (%d): %s' % (e.dll.decode(), len(names), ', '.join(names)))
code = {}
for s in pe.sections:
    if s.Characteristics & 0x20000000:
        code[s.Name.rstrip(b'\0').decode()] = (base + s.VirtualAddress, s.get_data()[:s.Misc_VirtualSize])
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.skipdata = True
calls = set(); stats = collections.Counter(); groups = collections.Counter(); total = 0
for name, (va, data) in code.items():
    n = 0
    for ins in md.disasm(data, va):
        n += 1; m = ins.mnemonic
        if m == 'call' and ins.op_str.startswith('0x'):
            calls.add(int(ins.op_str, 16))
        elif m == 'call': stats['indirect call'] += 1
        elif m == 'jmp' and not ins.op_str.startswith('0x'): stats['indirect jmp'] += 1
        if m.startswith('f'): groups['x87'] += 1
        elif re.match(r'(movss|movaps|movups|addps|mulps|addss|mulss|cvt|sqrtss|shufps|xorps|andps|rcpps|rsqrtps|movhlps|movlhps|minps|maxps|minss|maxss|subss|subps|divss|divps|comiss|ucomiss|unpck)', m): groups['sse'] += 1
        elif m.startswith('p') and 'mm' in ins.op_str: groups['mmx/sse2-int'] += 1
        elif m in ('rdtsc', 'cpuid', 'int3', 'in', 'out', 'sysenter'): groups[m] += 1
    print('section %s: %d instructions' % (name, n)); total += n
print('total instructions', total)
print('direct call targets (approx. functions):', len(calls))
print(dict(stats)); print(dict(groups))
