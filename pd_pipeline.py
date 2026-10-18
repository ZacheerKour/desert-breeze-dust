"""Phantom Dust: default.xbe -> generated C, using the exact stages pd_iterate.py used.

Stages (each fails loudly with its name):
  1 xbe_parser   default.xbe -> pd_analysis.json
  2 disasm       --seed-functions pd-game-v2/seed_functions.json
  3 extend       pd_extend_ends.py (function ends -> Ghidra contiguous ranges; pd-game-v2/ghidra_funcs.json)
  4 func_id
  5 abi_analysis
  6 recomp       --all --split 250 --manual-functions pd-game-v2/manual_functions.json
All intermediates go to --work; generated C goes to --gen (never the toolkit tree).
recomp_types.h is NOT produced by the lifter: it is copied from xboxrecomp/templates/runtime/recomp_types.h
(only if missing, matching the toolkit's "never overwritten" rule).
"""
import argparse, hashlib, os, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.abspath(__file__))


def run(name, cmd, cwd, log):
    t0 = time.time()
    print('[pipeline] %s ...' % name, flush=True)
    with open(log, 'w', encoding='utf-8', errors='replace') as f:
        p = subprocess.run(cmd, cwd=cwd, stdout=f, stderr=subprocess.STDOUT)
    print('[pipeline] %s rc=%d (%.0fs) log=%s' % (name, p.returncode, time.time() - t0, log), flush=True)
    if p.returncode != 0:
        print('[pipeline] FAILED at stage: %s' % name, file=sys.stderr)
        sys.exit(2)


def sha(path, h):
    with open(path, 'rb') as f:
        for blk in iter(lambda: f.read(1 << 20), b''):
            h.update(blk)


def input_stamp(xbe):
    """Hash of everything that determines the generated code: the XBE, seeds, manual list, Ghidra extents and the
    pipeline/lifter sources. Equal stamp + existing gen => regeneration is skipped (installer idempotency)."""
    h = hashlib.sha256()
    files = [xbe, os.path.join(ROOT, 'pd_extend_ends.py'), os.path.abspath(__file__)]
    proj = os.path.join(ROOT, 'pd-game-v2')
    files += [os.path.join(proj, n) for n in ('seed_functions.json', 'ghidra_funcs.json', 'manual_functions.json')]
    tk = os.path.join(ROOT, 'xboxrecomp')
    for d in ('xbe_parser', 'disasm', 'func_id', 'abi_analysis', 'recomp'):
        base = os.path.join(tk, 'tools', d)
        for dp, dn, fn in os.walk(base):
            dn[:] = sorted(x for x in dn if x not in ('__pycache__', 'output'))
            files += [os.path.join(dp, f) for f in sorted(fn) if f.endswith('.py') and not f.startswith('test_')]
    for f in files:
        h.update(os.path.relpath(f, ROOT).replace(os.sep, '/').encode())
        sha(f, h)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--xbe', default=os.path.join(ROOT, 'extracted', 'default.xbe'))
    ap.add_argument('--work', required=True, help='scratch/analysis directory')
    ap.add_argument('--gen', default=os.path.join(ROOT, 'pd-game-v2', 'src', 'recomp', 'gen'))
    ap.add_argument('--if-stale', action='store_true', help='skip when gen/.pd_stamp matches the current inputs')
    a = ap.parse_args()
    py = sys.executable
    tk = os.path.join(ROOT, 'xboxrecomp')
    proj = os.path.join(ROOT, 'pd-game-v2')
    seeds = os.path.join(proj, 'seed_functions.json')
    gfuncs = os.path.join(proj, 'ghidra_funcs.json')
    manual = os.path.join(proj, 'manual_functions.json')
    W = os.path.abspath(a.work)
    gen = os.path.abspath(a.gen)
    xbe = os.path.abspath(a.xbe)
    os.makedirs(W, exist_ok=True)
    stamp = input_stamp(xbe)
    stamp_file = os.path.join(gen, '.pd_stamp')
    if a.if_stale and os.path.exists(stamp_file) and os.path.exists(os.path.join(gen, 'recomp_dispatch.c')) \
            and open(stamp_file).read().strip() == stamp:
        print('[pipeline] generated code is up to date (stamp %s...); skipping' % stamp[:12])
        return
    L = lambda n: os.path.join(W, n + '.log')
    run('xbe_parser', [py, '-m', 'tools.xbe_parser', xbe, '--json', os.path.join(W, 'pd_analysis.json')], tk, L('xbe_parser'))
    run('disasm', [py, '-m', 'tools.disasm', xbe, '--analysis-json', os.path.join(W, 'pd_analysis.json'), '-o', os.path.join(W, 'disasm'),
                   '--seed-functions', seeds, '--force'], tk, L('disasm'))
    run('extend', [py, '-I', os.path.join(ROOT, 'pd_extend_ends.py'), xbe, W, gfuncs], tk, L('extend'))
    ext = os.path.join(W, 'disasm_ext')
    run('func_id', [py, '-m', 'tools.func_id', xbe, '--functions', os.path.join(ext, 'functions.json'), '--strings', os.path.join(ext, 'strings.json'),
                    '--xrefs', os.path.join(ext, 'xrefs.json'), '-o', os.path.join(W, 'func_id')], tk, L('func_id'))
    run('abi_analysis', [py, '-m', 'tools.abi_analysis', xbe, '--disasm-dir', ext, '--func-id-dir', os.path.join(W, 'func_id'),
                         '--output-dir', os.path.join(W, 'abi')], tk, L('abi'))
    os.makedirs(gen, exist_ok=True)
    for f in os.listdir(gen):
        fp = os.path.join(gen, f)
        if os.path.isfile(fp) and f.endswith(('.c', '.h')) and f != 'recomp_types.h':
            os.remove(fp)
    scratch = os.path.join(W, 'recomp-scratch')
    os.makedirs(scratch, exist_ok=True)
    run('recomp', [py, '-m', 'tools.recomp', xbe, '--all', '--split', '250', '--gen-dir', gen, '-o', scratch, '--disasm-dir', ext,
                   '--func-id-dir', os.path.join(W, 'func_id'), '--abi-dir', os.path.join(W, 'abi'), '--game-name', 'PhantomDust',
                   '--manual-functions', manual], tk, L('recomp'))
    rt = os.path.join(gen, 'recomp_types.h')
    if not os.path.exists(rt):
        shutil.copyfile(os.path.join(tk, 'templates', 'runtime', 'recomp_types.h'), rt)
        print('[pipeline] recomp_types.h provided from toolkit template')
    with open(stamp_file, 'w') as f:
        f.write(stamp + '\n')
    print('[pipeline] DONE: generated code in %s' % gen)


if __name__ == '__main__':
    main()
