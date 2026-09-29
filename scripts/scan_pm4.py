"""Find guest functions that build PM4 type-3 packet headers.

Scans the recompiled sources' assembly comments for `lis rX,imm` whose
upper half is 0xC0xx (type-3 header, count in bits 16-29) followed within a
few instructions by `ori`/`addi` on the same register that sets the opcode
byte. Reports each function with the opcodes it builds, grouped by whether
it lies inside the XDK D3D library range.
"""
import re, sys, glob, collections, json, os

D3D_LO, D3D_HI = 0x82914800, 0x82933240
OPCODES = {0x10:'NOP',0x22:'DRAW_INDX',0x36:'DRAW_INDX_2',0x2D:'SET_CONSTANT',0x2F:'LOAD_ALU_CONSTANT',
           0x27:'IM_LOAD',0x2B:'IM_LOAD_IMMEDIATE',0x3C:'WAIT_REG_MEM',0x3D:'MEM_WRITE',0x46:'EVENT_WRITE',
           0x3F:'INDIRECT_BUFFER',0x50:'EVENT_WRITE_ZPD',0x58:'EVENT_WRITE_SHD',0x59:'EVENT_WRITE_EXT',
           0x5A:'EVENT_WRITE_SHD',0x13:'INTERRUPT',0x21:'WAIT_FOR_IDLE',0x3E:'REG_RMW',0x23:'VIZ_QUERY',
           0x57:'INVALIDATE_STATE',0x54:'XE_SWAP',0x37:'INDIRECT_BUFFER_PFD',0x44:'COND_WRITE',0x48:'ME_INIT',
           0x2E:'SET_CONSTANT2',0x30:'SET_SHADER_CONSTANTS',0x31:'LOAD_CONSTANT_CONTEXT'}
func_re = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)')
lis_re = re.compile(r'//\s*lis (r\d+),(-?\d+)')
lo_re = re.compile(r'//\s*(ori|addi) (r\d+),(r\d+),(-?\d+)')
root = sys.argv[1]
found = collections.defaultdict(set)
for path in glob.glob(os.path.join(root, 'svr2011_recomp.*.cpp')):
    cur = None; pending = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = func_re.match(line)
            if m: cur = int(m.group(1), 16); pending = {}; continue
            m = lis_re.search(line)
            if m:
                hi = int(m.group(2)) & 0xFFFF
                if (hi & 0xC000) == 0xC000: pending[m.group(1)] = (hi, 6)
                else: pending.pop(m.group(1), None)
                continue
            m = lo_re.search(line)
            if m and m.group(3) in pending:
                hi, _ = pending.pop(m.group(3))
                lo = int(m.group(4)) & 0xFFFF
                if m.group(1) == 'addi' and int(m.group(4)) < 0: hi = (hi - 1) & 0xFFFF
                op = (lo >> 8) & 0x7F
                if (lo & 0xFE) == 0 and op in OPCODES:  # bit 0 = predicate
                    found[cur].add(OPCODES[op])
                continue
            if line.lstrip().startswith('//'):
                for r in list(pending):
                    h, n = pending[r]
                    if n <= 1: del pending[r]
                    else: pending[r] = (h, n - 1)
inside = {a: s for a, s in found.items() if D3D_LO <= a < D3D_HI}
outside = {a: s for a, s in found.items() if not (D3D_LO <= a < D3D_HI)}
print(f'functions building PM4 headers: {len(found)} (inside D3D range {len(inside)}, outside {len(outside)})')
print('\n-- inside D3D range --')
for a in sorted(inside): print(f'{a:08X}: {" ".join(sorted(inside[a]))}')
print('\n-- OUTSIDE D3D range --')
for a in sorted(outside): print(f'{a:08X}: {" ".join(sorted(outside[a]))}')
