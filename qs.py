#!/usr/bin/env python3
"""Quest sweep driver (worker q, 2026-10-19).
usage: python qs.py <exe-base> <tag> <id> [<id> ...]   (ids may be a-b ranges)
Drives a running instance (booted with run_i.sh <exe-base> <tag> RECOMP_FN_WATCH=5 ...) through quests via the
`quest <id>` console hook (works from the hub and from inside a battle), samples the window title (FPS/draws),
takes shots shot_qs_<id>_a/b.png and appends one line per quest to quest_sweep_raw.txt (id, status, t_first, fps range, notes).
On a dead process it reboots with run_i.sh (new tag) and continues. A hang = FPS 0.0 for 45 s in the play phase."""
import sys, os, time, subprocess, re, ctypes, ctypes.wintypes as wt, json
ROOT = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.join(ROOT, 'pd-game-v2')
exe, tag = sys.argv[1], sys.argv[2]
ids = []
for a in sys.argv[3:]:
    if '-' in a:
        lo, hi = a.split('-'); ids += list(range(int(lo), int(hi) + 1))
    else:
        ids.append(int(a))
ENVS = os.environ.get('QS_ENV', 'RECOMP_FN_WATCH=5 RECOMP_GUEST_LOCK=1 RECOMP_SAMPLE=1 PD_SAMPLE_DELAY=flag').split()
user32 = ctypes.windll.user32

def pids():
    out = subprocess.run(['powershell', '-NoProfile', '-Command', f'(Get-Process {exe} -ErrorAction SilentlyContinue).Id'],
                         capture_output=True, text=True).stdout.split()
    return [int(x) for x in out]

def title():
    p = set(pids())
    res = []
    def cb(h, _):
        pid = wt.DWORD(); user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
        if pid.value in p and user32.IsWindowVisible(h):
            n = user32.GetWindowTextLengthW(h)
            if n:
                b = ctypes.create_unicode_buffer(n + 1); user32.GetWindowTextW(h, b, n + 1); res.append(b.value)
        return True
    P = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    user32.EnumWindows(P(cb), 0)
    for t in res:
        m = re.search(r'FPS: ([\d.]+)(?: \(game ([\d.]+)\))? \| draws: (\d+)', t)
        if m: return float(m.group(2) or m.group(1)), int(m.group(3))
    return None

def pad(s):
    with open(os.path.join(GAME, f'pad_live_{tag}.txt'), 'a') as f: f.write(s + '\n')
def con(s):
    with open(os.path.join(GAME, f'mem_live_{tag}.txt'), 'a') as f: f.write(s + '\n')
def shot(name):
    subprocess.run(['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', os.path.join(ROOT, 'capwin_i.ps1'), exe,
                    os.path.join(GAME, name)], capture_output=True)
def errtxt():
    try: return open(os.path.join(GAME, f'run_{tag}.err'), 'rb').read().decode('latin1')
    except Exception: return ''
def log(s):
    with open(os.path.join(ROOT, 'quest_sweep_raw.txt'), 'a') as f: f.write(s + '\n')
    print(s, flush=True)

def reboot():
    global tag
    n = int(re.sub(r'\D', '', tag) or 0) + 1
    tag = 'q%d' % n if tag.startswith('q') else tag + 'r'
    log(f'# reboot -> tag {tag}')
    subprocess.run(['bash', os.path.join(ROOT, 'run_i_fast.sh'), exe, tag] + ENVS, cwd=ROOT)
    pad('b:200'); time.sleep(4)

PLAY = ['lup:1500', 'rt:250', 'a:200', 'lup+lright:1200', 'x:200', 'y:200', 'b:200', 'lup+lleft:1200', 'a:200', 'rt:250', 'lup:1500', 'x:200']

IN_BATTLE = False
def go_hub():
    base = len(errtxt()); con('hub'); t0 = time.time()
    while time.time() - t0 < 120:
        time.sleep(2)
        if not pids(): return 'crash'
        if '[HUB] hub up' in errtxt()[base:]:
            time.sleep(float(os.environ.get('QS_HUBWAIT', '0'))); return 'ok'
        pad('a:100') if time.time() - t0 > 30 and int(time.time() - t0) % 7 == 0 else None
    return 'hub-timeout'

def run_quest(q, mode='chain'):
    global IN_BATTLE
    hubst = 'ok'
    if IN_BATTLE:
        hubst = go_hub(); IN_BATTLE = False
        if hubst != 'ok':
            log(json.dumps({'id': q, 'mode': mode, 'status': hubst, 'notes': ['hub step after the previous quest failed']})); return hubst
    base = len(errtxt())
    t0 = time.time(); con(f'quest {q}')
    fps = []; first = None; status = 'ok'; notes = []
    # load phase
    started = False
    while time.time() - t0 < 90:
        time.sleep(2)
        if not pids(): status = 'crash'; break
        e = errtxt()[base:]
        if 'stage torn down; starting quest' in e: started = True
        t = title()
        if t and first is None and started and t[1] > 100: first = time.time() - t0
        if first is not None: break
        pad('a:150')
    if status != 'crash' and first is None:
        status = 'noload' if not started else 'load-timeout'
        if started: shot(f'shot_qs_{q}_a.png')
    tf = time.time()
    if started: IN_BATTLE = True
    if status in ('ok',):
        # settle + dialogue advance
        while time.time() - tf < 15:
            pad('a:150'); time.sleep(3.2)
            t = title()
            if t: fps.append(t)
        shot(f'shot_qs_{q}_a.png')
        zero = 0; i = 0; tp = time.time()
        while time.time() - tp < 55:
            pad(PLAY[i % len(PLAY)]); i += 1; time.sleep(1.6)
            if not pids(): status = 'crash'; break
            t = title()
            if t:
                fps.append(t); zero = zero + 1 if t[0] == 0.0 else 0
                if zero * 2.2 > 45: status = 'hang'; break
            else: zero += 1
        if status == 'ok': shot(f'shot_qs_{q}_b.png')
    e = errtxt()[base:]
    for m in re.findall(r'\[CRASH2?\][^\n]*', e)[:2]: notes.append(m.strip()[:160])
    if status == 'crash':
        notes += [l[:160] for l in e.strip().split('\n')[-3:]]
    if status == 'hang':
        shot(f'shot_qs_{q}_b.png')
        open(os.path.join(GAME, 'sample.flag'), 'w').close(); time.sleep(25)
        subprocess.run(['bash', '-c', f'cp "{GAME}/run_{tag}.err" "{GAME}/hang_q{q}_{tag}.err"'])
        notes.append(f'sampled: hang_q{q}_{tag}.err')
    fv = [x[0] for x in fps if x[0] > 0] or [0]
    dv = [x[1] for x in fps] or [0]
    log(json.dumps({'id': q, 'mode': mode, 'status': status, 't_first': None if first is None else round(first), 'fps_min': min(fv), 'fps_max': max(fv),
                    'draws_max': max(dv), 'notes': notes}))
    return status

for q in ids:
    if not pids():
        reboot(); IN_BATTLE = False
    st = run_quest(q, 'chain')
    if st in ('crash', 'hang', 'noload', 'load-timeout', 'hub-timeout'):
        subprocess.run(['taskkill', '/F', '/IM', exe + '.exe'], capture_output=True)
        time.sleep(3)
        reboot(); IN_BATTLE = False
        # retry once as the FIRST quest after a fresh boot (separates battle->battle chaining artefacts from quest bugs)
        st2 = run_quest(q, 'fresh-retry')
        if st2 in ('crash', 'hang', 'noload', 'load-timeout', 'hub-timeout'):
            subprocess.run(['taskkill', '/F', '/IM', exe + '.exe'], capture_output=True)
            time.sleep(3)
            reboot(); IN_BATTLE = False
