"""Parser for server_perf_interleave.ps1: assigns every profile window (SVPROF / BOTPROF / GAMEPROF) to
the setting that was active for the whole window ("ILCFG <name>" markers), drops windows that overlap
a switch, averages per setting and appends one JSON line per setting.
usage: server_perf_interleave.py <qconsole.log> <tag> <map> <bots> <svfps> <windowSec> <results.jsonl>"""
import json, re, sys
from datetime import datetime

log, tag, mapname, bots, svfps, win, results = sys.argv[1:8]
win = float(win)

def ts(line):
    try:
        return datetime.strptime(line[:19], '%Y-%m-%d %H:%M:%S').timestamp()
    except ValueError:
        return None

markers = []      # (time, name)
pend = {'gz': {}, 'bz': {}}   # BOTPROF / GAMEPROF lines come before the SVPROF line of the same window
windows = []      # dicts, keyed by end time
cur = None
for raw in open(log, encoding='latin-1'):
    line = raw.rstrip('\r\n')
    t = ts(line)
    if t is None:
        continue
    msg = line[20:]
    m = re.match(r'^ILCFG (\S+)', msg)
    if m:
        markers.append((t, m.group(1)))
        continue
    m = re.match(r'^SVPROF window=\S+ frames=(\d+) gameframes=\d+ bots=(\d+)', msg)
    if m:
        cur = {'end': t, 'frames': int(m.group(1)), 'bots': int(m.group(2)), 'gz': {}, 'bz': {}, 'parts': {}}
        if pend.get('t') is not None and t - pend['t'] <= 2:
            cur.update({k: v for k, v in pend.items() if k != 't'})
        pend = {'gz': {}, 'bz': {}}
        windows.append(cur)
        continue
    if msg.startswith('BOTPROF') or msg.startswith('BOTZONE') or msg.startswith('GAMEPROF') or msg.startswith('GAMEZONE'):
        tgt = pend
        if pend.get('t') is None:
            pend['t'] = t
    else:
        tgt = cur
    if tgt is None:
        continue
    m = re.match(r'^SVPROF ms/frame avg\(max\): total ([\d.]+) \(([\d.]+)\)\s+botai ([\d.]+) \(([\d.]+)\)\s+game ([\d.]+) \(([\d.]+)\)\s+snap ([\d.]+)', msg)
    if m:
        cur.update(total=float(m.group(1)), totalMax=float(m.group(2)), botai=float(m.group(3)), game=float(m.group(5)), gameMax=float(m.group(6)), snap=float(m.group(7)))
        continue
    m = re.match(r'^SVPROF percentiles ms: total p50 ([\d.]+) p95 ([\d.]+) p99 ([\d.]+)\s+botai p50 ([\d.]+) p95 ([\d.]+) p99 ([\d.]+)', msg)
    if m:
        cur.update(p50=float(m.group(1)), p95=float(m.group(2)), p99=float(m.group(3)), botaiP99=float(m.group(6)))
        continue
    m = re.match(r'^SVPROF percentiles ms: game p50 ([\d.]+) p99 ([\d.]+)\s+snap p50 ([\d.]+) p99 ([\d.]+)', msg)
    if m:
        cur.update(gameP99=float(m.group(2)), snapP99=float(m.group(4)))
        continue
    m = re.match(r'^SVPROF part (.+?)\s+ms/frame avg ([\d.]+) p99 ([\d.]+) max ([\d.]+)\s+calls/frame ([\d.]+)', msg)
    if m:
        cur['parts'][m.group(1).strip()] = [float(m.group(2)), float(m.group(3)), float(m.group(4)), float(m.group(5))]
        continue
    m = re.match(r'^SVPROF (botai|game )\s*traces/frame ([\d.]+)\s+trace ms/frame ([\d.]+)', msg)
    if m:
        k = m.group(1).strip()
        cur[k + 'Traces'] = float(m.group(2))
        cur[k + 'TraceMs'] = float(m.group(3))
        continue
    m = re.match(r'^BOTPROF parallel sense .*?traces made early ([\d.]+)/frame, used ([\d.]+)', msg)
    if m:
        tgt.update(early=float(m.group(1)), used=float(m.group(2)))
        continue
    m = re.match(r'^BOTPROF frame ai kcycles ([\d.]+)', msg)
    if m:
        tgt['aiKc'] = float(m.group(1))
        continue
    m = re.match(r'^BOTZONE (.+?)\s+self ms/frame\s+([\d.]+)', msg)
    if m:
        tgt['bz'][m.group(1).strip()] = float(m.group(2))
        continue
    m = re.match(r'^GAMEPROF window=\S+ frames=\d+ .*?game ms/frame avg ([\d.]+) p99 ([\d.]+) max ([\d.]+)', msg)
    if m:
        tgt.update(gf=float(m.group(1)), gfP99=float(m.group(2)), gfMax=float(m.group(3)))
        continue
    m = re.match(r'^GAMEZONE (.+?)\s+self ms/frame avg\s+([\d.]+) p99\s+([\d.]+) max\s+([\d.]+)', msg)
    if m:
        tgt['gz'][m.group(1).strip()] = [float(m.group(2)), float(m.group(3)), float(m.group(4))]
        continue

def setting_for(w):
    start, end = w['end'] - win, w['end']
    name = None
    for i, (t, n) in enumerate(markers):
        nxt = markers[i + 1][0] if i + 1 < len(markers) else 1e18
        if t + 1 <= start and nxt >= end:
            name = n
    return name

groups = {}
for w in windows:
    if 'total' not in w:
        continue
    n = setting_for(w)
    if n and n != 'end':
        groups.setdefault(n, []).append(w)

def wavg(ws, k):
    v = [(w[k], w['frames']) for w in ws if k in w]
    s = sum(f for _, f in v)
    return round(sum(x * f for x, f in v) / s, 4) if s else None

def mean(ws, k):
    v = [w[k] for w in ws if k in w]
    return round(sum(v) / len(v), 3) if v else None

def mx(ws, k):
    v = [w[k] for w in ws if k in w]
    return max(v) if v else None

def zones(ws, key, idx=None):
    acc = {}
    for w in ws:
        for z, v in w[key].items():
            acc.setdefault(z, []).append(v if idx is None else v[idx])
    return {z: round(sum(v) / len(ws), 4) for z, v in sorted(acc.items(), key=lambda kv: -sum(kv[1]))}

out = open(results, 'a', encoding='utf-8')
print('%-8s %3s  %7s %6s %6s | %6s %6s %6s | %6s %6s | %5s %5s' % ('setting', 'win', 'frame', 'p99', 'max', 'botai', 'game', 'snap', 'aiTr', 'gTr', 'early', 'used'))
for n, ws in groups.items():
    r = {'tag': tag, 'setting': n, 'map': mapname, 'botsAsked': int(bots), 'bots': mx(ws, 'bots'), 'svFps': int(svfps), 'windows': len(ws),
         'frames': sum(w['frames'] for w in ws), 'frameMs': wavg(ws, 'total'), 'frameP50': mean(ws, 'p50'), 'frameP99': mean(ws, 'p99'),
         'frameMaxMs': mx(ws, 'totalMax'), 'botaiMs': wavg(ws, 'botai'), 'botaiP99': mean(ws, 'botaiP99'), 'gameMs': wavg(ws, 'game'),
         'gameP99': mean(ws, 'gameP99'), 'gameMaxMs': mx(ws, 'gameMax'), 'snapMs': wavg(ws, 'snap'), 'snapP99': mean(ws, 'snapP99'),
         'aiTraces': wavg(ws, 'botaiTraces'), 'aiTraceMs': wavg(ws, 'botaiTraceMs'), 'gameTraces': wavg(ws, 'gameTraces'), 'gameTraceMs': wavg(ws, 'gameTraceMs'),
         'early': wavg(ws, 'early'), 'used': wavg(ws, 'used'), 'aiKc': wavg(ws, 'aiKc'), 'gf': wavg(ws, 'gf'), 'gfP99': mean(ws, 'gfP99'),
         'gameZonesAvg': zones(ws, 'gz', 0), 'gameZonesP99': zones(ws, 'gz', 1), 'gameZonesMax': {z: max(w['gz'][z][2] for w in ws if z in w['gz']) for z in zones(ws, 'gz', 0)},
         'botZones': zones(ws, 'bz'), 'partsAvg': zones(ws, 'parts', 0), 'partsP99': zones(ws, 'parts', 1),
         'partsMax': {z: max(w['parts'][z][2] for w in ws if z in w['parts']) for z in zones(ws, 'parts', 0)}}
    out.write(json.dumps(r) + '\n')
    print('%-8s %3d  %7.3f %6.2f %6.1f | %6.3f %6.3f %6.3f | %6.0f %6.0f | %5s %5s' % (n, len(ws), r['frameMs'] or 0, r['frameP99'] or 0, r['frameMaxMs'] or 0,
        r['botaiMs'] or 0, r['gameMs'] or 0, r['snapMs'] or 0, r['aiTraces'] or 0, r['gameTraces'] or 0, r['early'], r['used']))
out.close()
