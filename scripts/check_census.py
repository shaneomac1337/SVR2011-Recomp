"""Check a --svr_d3d_census / --gpu_draw_census session log.

For each 60-frame window: DRAW_INDX executed by the GPU must equal the
game's DrawIndexedVertices + DrawVerticesUP calls, and DRAW_INDX_2 must equal
Clear rectangles + Resolves + other D3D draw builders + the fixed point-list
filler buffer (24 draws per replay). Also reports memexport use.
Usage: check_census.py <runtime.log>
"""
import re, sys

def kv(line):
    return {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)(?=[\s)]|$)', line)}

lines = open(sys.argv[1], encoding='utf-8', errors='replace').read().splitlines()
guest = {}; gpu = {}; vk = {}
for l in lines:
    if 'draw-census guest' in l: d = kv(l); guest[d['presents']] = d
    elif 'draw-census gpu swaps' in l: d = kv(l); gpu[d['swaps']] = d
    elif 'draw-census vulkan' in l: d = kv(l); vk[d['swaps']] = d
frames = sorted(set(guest) & set(gpu) & set(vk))
bad = 0; prev = None
for f in frames:
    g, p, v = guest[f], gpu[f], vk[f]
    if prev is not None:
        G, P, V = guest[prev], gpu[prev], vk[prev]
        d = lambda a, b, k: a[k] - b[k]
        di = d(p, P, 'draw_indx'); gi = d(g, G, 'indexed') + d(g, G, 'up')
        d2 = d(p, P, 'draw_indx_2')
        known2 = d(g, G, 'clear_rect') + d(g, G, 'other_draws') + d(g, G, 'resolve')
        points = d(v, V, 'points')
        ok = di == gi and d2 - known2 == points and points % 24 == 0
        # Guest and GPU logs are taken a frame or two apart; allow one window of slack.
        if not ok:
            bad += 1
            print(f'frames {prev}-{f}: DRAW_INDX gpu={di} guest={gi} | DRAW_INDX_2 gpu={d2} '
                  f'known={known2} points={points} | memexport vs/ps='
                  f'{d(v, V, "memexport_vs")}/{d(v, V, "memexport_ps")}')
    prev = f
last = frames[-1] if frames else None
if last:
    g, p, v = guest[last], gpu[last], vk[last]
    print(f'{len(frames)} windows up to frame {last}, {bad} mismatched')
    print(f'totals: DRAW_INDX gpu={p["draw_indx"]} guest={g["indexed"] + g["up"]}; '
          f'DRAW_INDX_2 gpu={p["draw_indx_2"]} = clear {g["clear_rect"]} + resolve {g["resolve"]} '
          f'+ other {g["other_draws"]} + points {v["points"]}; memexport vs={v["memexport_vs"]} '
          f'ps={v["memexport_ps"]}; shaders vs={g["create_vs"]} ps={g["create_ps"]}')
