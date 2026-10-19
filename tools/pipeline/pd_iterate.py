"""Automated bring-up loop for the Phantom Dust xboxrecomp scratch project (pd-game-v2).

Each cycle: run pd_recomp.exe (time-limited), harvest `[ICALL] Failed to resolve VA` targets from stderr,
seed them through tools.seed_from_log (its two safety gates), re-run disasm/func_id/abi_analysis/lifter,
rebuild, and run again. Stops when no new seedable target appears or after MAX_CYCLES.
Writes docs/iterate_log.md. Never leaves the game running (kills by image name after each run).
Only touches: recomp-analysis/, pd-game-v2/ (generated code + build), logs. Does not modify xboxrecomp/ or extracted/.
"""
import os, re, shutil, subprocess, sys, time, json

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOLKIT = os.path.join(ROOT, 'xboxrecomp')
PY = os.path.join(ROOT, '.venv-recomp', 'Scripts', 'python.exe')
XBE = os.path.join(ROOT, 'extracted', 'default.xbe')
AN = os.path.join(ROOT, 'recomp-analysis')
PROJ = os.path.join(ROOT, 'pd-game-v2')
GEN = os.path.join(PROJ, 'src', 'recomp', 'gen')
LOG = os.path.join(ROOT, 'docs', 'iterate_log.md')
MAX_CYCLES = int(sys.argv[1]) if len(sys.argv) > 1 else 10
RUN_SECS = int(os.environ.get("PD_RUN_SECS", "60"))

def sh(cmd, cwd=None, timeout=1800, out=None):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout, shell=isinstance(cmd, str))
    if out:
        open(out, 'w', encoding='utf-8').write((p.stdout or '') + (p.stderr or ''))
    return p

def kill_game():
    subprocess.run('taskkill /F /IM pd_recomp.exe', capture_output=True, shell=True)

def run_game(tag):
    err = os.path.join(PROJ, 'run_%s_err.txt' % tag); out = os.path.join(PROJ, 'run_%s_out.txt' % tag)
    with open(err, 'w') as fe, open(out, 'w') as fo:
        p = subprocess.Popen([os.path.join(PROJ, 'build', 'pd_recomp.exe')], cwd=PROJ, stdout=fo, stderr=fe)
        t0 = time.time()
        while p.poll() is None and time.time() - t0 < RUN_SECS:
            time.sleep(0.5)
        timed_out = p.poll() is None
        if timed_out:
            p.kill()
    kill_game(); time.sleep(1)
    left = subprocess.run('tasklist /FI "IMAGENAME eq pd_recomp.exe"', capture_output=True, text=True, shell=True).stdout
    return err, timed_out, ('pd_recomp.exe' in left)

def summarize(err, timed_out):
    t = open(err, encoding='utf-8', errors='replace').read()
    fails = re.findall(r'Failed to resolve VA (0x[0-9A-Fa-f]+)', t)
    uniq = sorted(set(int(x, 16) for x in fails))
    crash = len(re.findall(r'\[CRASH\]', t))
    ksum = re.findall(r'total calls: (\d+)', t)
    ksum2 = re.findall(r'KERNEL\] summary: (\d+) total calls', t)
    first_crash = re.search(r'\[CRASH\][^\n]*\n(?:[^\n]*\n){0,3}', t)
    return dict(lines=t.count('\n'), failed_total=len(fails), failed_unique=uniq, crash=crash, timed_out=timed_out,
                kernel_summaries=ksum2[-1:] , first_crash=(first_crash.group(0).strip()[:400] if first_crash else ''),
                tail=t.strip().splitlines()[-4:])

def stage(name, cmd, log):
    t0 = time.time(); p = sh(cmd, cwd=TOOLKIT, out=log); return p.returncode, time.time() - t0

def regen(cycle):
    out = os.path.join(AN, 'cycle%d_' % cycle)
    steps = []
    rc, s = stage('disasm', [PY, '-m', 'tools.disasm', XBE, '--analysis-json', os.path.join(AN, 'pd_analysis.json'), '-o', os.path.join(AN, 'disasm'),
                             '--seed-functions', os.path.join(AN, 'seed_functions.json'), '--force'], out + 'disasm.log'); steps.append(('disasm', rc))
    rc, s = stage('extend', [PY, '-I', os.path.join(HERE, 'pd_extend_ends.py')], out + 'extend.log'); steps.append(('extend', rc))
    rc, s = stage('func_id', [PY, '-m', 'tools.func_id', XBE, '--functions', os.path.join(AN, 'disasm_ext', 'functions.json'), '--strings', os.path.join(AN, 'disasm_ext', 'strings.json'),
                              '--xrefs', os.path.join(AN, 'disasm_ext', 'xrefs.json'), '-o', os.path.join(AN, 'func_id')], out + 'func_id.log'); steps.append(('func_id', rc))
    rc, s = stage('abi', [PY, '-m', 'tools.abi_analysis', XBE, '--disasm-dir', os.path.join(AN, 'disasm_ext'), '--func-id-dir', os.path.join(AN, 'func_id'),
                          '--output-dir', os.path.join(AN, 'abi')], out + 'abi.log'); steps.append(('abi', rc))
    # clear generated code (literal project path) and re-lift
    if os.path.isdir(GEN):
        for f in os.listdir(GEN):
            fp = os.path.join(GEN, f)
            if os.path.isfile(fp) and f.endswith(('.c', '.h')):
                os.remove(fp)
    scratch = os.path.join(ROOT, 'recomp-scratch5'); os.makedirs(scratch, exist_ok=True)
    rc, s = stage('lift', [PY, '-m', 'tools.recomp', XBE, '--all', '--split', '250', '--gen-dir', GEN, '-o', scratch, '--disasm-dir', os.path.join(AN, 'disasm_ext'),
                           '--func-id-dir', os.path.join(AN, 'func_id'), '--abi-dir', os.path.join(AN, 'abi'), '--game-name', 'PhantomDust', '--manual-functions', os.path.join(PROJ, 'manual_functions.json')], out + 'lift.log'); steps.append(('lift', rc))
    summ = json.load(open(os.path.join(scratch, 'summary.json')))
    b = sh('cmd /c ".\\build_env.bat build"', cwd=PROJ, out=os.path.join(PROJ, 'build_cycle%d.log' % cycle))
    btxt = open(os.path.join(PROJ, 'build_cycle%d.log' % cycle), encoding='utf-8', errors='replace').read()
    berr = len(re.findall(r' error C\d+|LNK\d+|FAILED', btxt))
    return steps, summ, berr

def main():
    open(LOG, 'w', encoding='utf-8').write('# pd-game-v2 automated bring-up loop\n\nStarted %s. Each cycle: run (<=%ds), harvest failed ICALL targets, seed (gated), regen, rebuild.\n\n' % (time.ctime(), RUN_SECS))
    kill_game()
    for cycle in range(1, MAX_CYCLES + 1):
        err, timed_out, left = run_game('c%d' % cycle)
        s = summarize(err, timed_out)
        entry = '## Cycle %d\n- run: lines=%d, timed_out=%s, game still running after kill=%s\n- failed ICALL: %d total, %d unique %s\n- CRASH lines: %d\n- kernel summary calls: %s\n' % (
            cycle, s['lines'], s['timed_out'], left, s['failed_total'], len(s['failed_unique']), [hex(x) for x in s['failed_unique'][:12]], s['crash'], s['kernel_summaries'])
        if s['first_crash']:
            entry += '- first crash:\n```\n%s\n```\n' % s['first_crash']
        entry += '- tail: `%s`\n' % ' | '.join(x.strip()[:90] for x in s['tail'])
        open(LOG, 'a', encoding='utf-8').write(entry)
        if left:
            open(LOG, 'a').write('\n**STOP: game process still present after kill.**\n'); break
        if not s['failed_unique']:
            open(LOG, 'a').write('\nNo failed ICALL targets this cycle; stopping loop (see crash/tail above).\n'); break
        # seed
        n0 = len(json.load(open(os.path.join(AN, 'seed_functions.json'))))
        p = sh([PY, '-m', 'tools.seed_from_log', err, XBE, '--functions', os.path.join(AN, 'disasm', 'functions.json'), '--seeds', os.path.join(AN, 'seed_functions.json'),
                '--analysis-json', os.path.join(AN, 'pd_analysis.json')], cwd=TOOLKIT, out=os.path.join(AN, 'cycle%d_seed.log' % cycle))
        n1 = len(json.load(open(os.path.join(AN, 'seed_functions.json'))))
        open(LOG, 'a', encoding='utf-8').write('- seeded: %d new (total %d)\n' % (n1 - n0, n1))
        if n1 == n0:
            open(LOG, 'a').write('\nNothing new could be seeded (targets rejected by the gates or already known); stopping.\n'); break
        steps, summ, berr = regen(cycle)
        u = summ.get('unimplemented', {})
        open(LOG, 'a', encoding='utf-8').write('- regen: %s; lifted %d/%d functions (failed %d), unresolved stubs %d, untranslated sites %d; build errors %d\n\n' % (
            steps, summ['translated'], summ['total'], summ['failed'], summ['unresolved_stubs'], sum(len(v) for v in u.values()), berr))
        if berr:
            open(LOG, 'a').write('**STOP: build errors.**\n'); break
    open(LOG, 'a').write('\nFinished %s.\n' % time.ctime())

if __name__ == '__main__':
    main()
