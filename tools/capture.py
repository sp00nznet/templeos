"""Build the ahead-of-time cache: lift, build, run hidden, repeat.

  python tools/capture.py <TempleOS.ISO> [--rounds N] [--secs S] [--build DIR]

Each run boots the lifted system offscreen and muted. It stops when it reaches
code that has not been lifted yet: a function discovery missed (exit 20, the
address is in work/seeds.txt) or a module TempleOS loaded itself (exit 22,
its bytes are in work/jit/). Then tools/gen.py lifts what was recorded, the
build picks it up, and the next run gets further. It ends when a run lasts its
full time, or stops for another reason (which it prints).
"""
import os, sys, subprocess, argparse, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def exe_path(build):
    for p in (os.path.join(build, 'Release', 'templeos.exe'), os.path.join(build, 'templeos.exe'),
              os.path.join(build, 'templeos')):
        if os.path.exists(p):
            return p
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('iso')
    ap.add_argument('--rounds', type=int, default=60)
    ap.add_argument('--secs', type=float, default=20)
    ap.add_argument('--build', default=os.path.join(ROOT, 'build'))
    ap.add_argument('--keys', nargs=2, action='append', default=[], metavar=('TEXT', 'SECS'),
                    help='type TEXT at SECS (repeatable), to capture what commands compile')
    a = ap.parse_args()
    env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy')
    os.makedirs(os.path.join(ROOT, 'work'), exist_ok=True)
    for n in range(1, a.rounds + 1):
        subprocess.check_call([sys.executable, os.path.join(ROOT, 'tools', 'gen.py'), a.iso])
        r = subprocess.run(['cmake', '--build', a.build, '--config', 'Release', '-j'],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode:
            print('\n'.join(l for l in r.stdout.splitlines() if 'error' in l)[:4000])
            return 1
        exe = exe_path(a.build)
        shot = os.path.join(ROOT, 'work', 'capture.png')
        try:
            keys = [x for k in a.keys for x in ('--keys', k[0].replace('\\n', '\n'), k[1])]
            p = subprocess.run([exe, '--hidden'] + keys + ['--shot', shot, str(a.secs), '--quit', str(a.secs + 0.5), a.iso],
                               env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                               timeout=a.secs + 120)
            out, rc = p.stdout, p.returncode
        except subprocess.TimeoutExpired:
            out, rc = '', -1
        tail = [l for l in out.splitlines() if l.strip()][-3:]
        print('[round %d] exit %d: %s' % (n, rc, ' | '.join(tail)), flush=True)
        if rc not in (20, 22):
            return 0 if rc == 0 else rc
    return 0


if __name__ == '__main__':
    sys.exit(main())
