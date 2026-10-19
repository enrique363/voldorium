"""Builds Voldorium from your own SoulCalibur disc dump, every step in one command.

    python -I tools/build.py --gdi path/to/soulcalibur.gdi [--download] [--player-dir DIR]

Steps (each skipped when its output is up to date):
  1. extract   the boot files from the dump (tools/extract_boot.py, checks the SHA-1)
  2. translate the game code into C (port/sh4c/sweep.py -> port/build/sweep)
  3. ggpo      the rollback library (third_party/ggpo)
  4. game      compile and link Voldorium.exe and m1.exe (port/m1)
  5. player    the player folder: the executables, SDL2.dll, settings, README.txt

Tools: llvm-mingw, SDL2 (mingw development files), and two files from Flycast (the SH-4 FSCA
sine table header and the replacement BIOS font). Each is looked for in this order: the
variables LLVM_MINGW, SDL2_DIR, FLYCAST (environment or local.env at the repository root), the
engines/ folder next to the repository, then toolchain/ in the repository. --download fetches
whatever is missing into toolchain/, at the exact versions below, and checks their SHA-256: every
builder then has the same compiler, so every build of the same sources is the same program (the
online lobby only matches identical builds).

Only the game's own code comes from the disc, on your machine; nothing of the game is downloaded
or stored in the repository.
"""
import argparse
import concurrent.futures
import hashlib
import io
import os
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile

SC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = os.path.join(SC, 'port')
CACHE = os.path.join(SC, 'toolchain')
LOCAL = os.environ.get('LOCALAPPDATA') or os.path.expanduser('~')
OUT = os.path.join(LOCAL, 'sc-m1')          # objects and executables (outside the repository)

# pinned downloads: name -> (url, sha256, folder inside toolchain/ once unpacked)
FLYCAST_COMMIT = '5aa091fde632fb332c8d8c34e280d62dc951954c'
PINNED = {
    'llvm-mingw': ('https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/llvm-mingw-20260922-ucrt-x86_64.zip',
                   None, 'llvm-mingw-20260922-ucrt-x86_64'),
    'sdl2': ('https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.zip',
             None, 'SDL2-2.32.10'),
    'fsca-table.h': ('https://raw.githubusercontent.com/flyinghead/flycast/%s/core/hw/sh4/fsca-table.h' % FLYCAST_COMMIT,
                     '2ab87d6df67546c8a27febafb2c2e520fd1266b91633e4edfa8ec48b58218300', 'flycast/core/hw/sh4'),
    'biosfont.bin.zip': ('https://raw.githubusercontent.com/flyinghead/flycast/%s/fonts/biosfont.bin.zip' % FLYCAST_COMMIT,
                         'e996bc0f9854fbe7ad057948c8cda6ff9ef9b4b6d0347c92f1ecad71a92214ce', 'flycast/fonts'),
}

GAME_SOURCES = ['m1', 'hw', 'hle', 'threads', 'maple', 'ta', 'tex', 'render', 'state', 'netplay', 'aica', 'arm7',
                'config', 'online', 'artdump', 'artswap', 'png']


def say(msg):
    print(msg, flush=True)


def fail(msg):
    say('\nERROR: ' + msg)
    sys.exit(1)


def local_env():
    """KEY=VALUE lines of local.env (this machine's tool locations; not in git)."""
    env = {}
    path = os.path.join(SC, 'local.env')
    if os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            line = line.split('#')[0].strip()
            if '=' in line:
                k, v = line.split('=', 1)
                env[k.strip()] = os.path.expandvars(v.strip().strip('"').replace('$HOME', os.path.expanduser('~')))
    return env


def download(name):
    url, sha, _ = PINNED[name]
    say('  downloading %s' % url)
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=60) as r:
                data = r.read()
            break
        except Exception as e:  # network hiccup: try again
            if attempt == 2:
                fail('could not download %s (%s). Check the internet connection and run the builder again.' % (url, e))
            time.sleep(3)
    digest = hashlib.sha256(data).hexdigest()
    if sha and digest != sha:
        fail('%s does not match its pinned SHA-256 (got %s). The download was damaged or changed; nothing was used.' % (name, digest))
    if not sha:
        say('  (no pinned SHA-256 yet for %s: %s)' % (name, digest))
    say('  %s: %.1f MB, SHA-256 ok' % (name, len(data) / 1e6))
    return data


def tool_dirs(allow_download):
    """(llvm-mingw, SDL2 x86_64 folder, folder holding fsca-table.h, biosfont.bin.zip or None)"""
    env = dict(local_env(), **{k: v for k, v in os.environ.items() if k in ('LLVM_MINGW', 'SDL2_DIR', 'FLYCAST')})
    engines = os.path.join(os.path.dirname(SC), 'engines')

    def first(*cands):
        for c in cands:
            if c and os.path.exists(c):
                return c
        return None

    llvm = first(env.get('LLVM_MINGW'), os.path.join(engines, PINNED['llvm-mingw'][2]), os.path.join(CACHE, PINNED['llvm-mingw'][2]))
    sdl = first(env.get('SDL2_DIR'), os.path.join(engines, 'SDL2-2.32.10', 'x86_64-w64-mingw32'),
                os.path.join(CACHE, PINNED['sdl2'][2], 'x86_64-w64-mingw32'))
    fly = env.get('FLYCAST') or os.path.join(engines, 'flycast')
    fsca = first(os.path.join(fly, 'core', 'hw', 'sh4', 'fsca-table.h'), os.path.join(CACHE, 'flycast', 'core', 'hw', 'sh4', 'fsca-table.h'))
    font = first(os.path.join(fly, 'fonts', 'biosfont.bin.zip'), os.path.join(CACHE, 'flycast', 'fonts', 'biosfont.bin.zip'))
    missing = [n for n, v in (('llvm-mingw', llvm), ('sdl2', sdl), ('fsca-table.h', fsca), ('biosfont.bin.zip', font)) if not v]
    if missing and not allow_download:
        fail('missing tools: %s. Run with --download to fetch them into toolchain/ (or set LLVM_MINGW, SDL2_DIR, FLYCAST).'
             % ', '.join(missing))
    for name in missing:
        say('Getting %s...' % name)
        data = download(name)
        dest = os.path.join(CACHE, PINNED[name][2])
        os.makedirs(dest, exist_ok=True)
        if name in ('llvm-mingw', 'sdl2'):
            zipfile.ZipFile(io.BytesIO(data)).extractall(os.path.dirname(dest) if name == 'llvm-mingw' else dest)
            if name == 'sdl2':   # the zip holds SDL2-2.32.10/...: lift it one level
                inner = os.path.join(dest, PINNED['sdl2'][2])
                if os.path.isdir(inner):
                    for x in os.listdir(inner):
                        shutil.move(os.path.join(inner, x), os.path.join(dest, x))
                    os.rmdir(inner)
        else:
            open(os.path.join(dest, name), 'wb').write(data)
    if missing:
        return tool_dirs(False)
    return llvm, sdl, os.path.dirname(fsca), font


def run(cmd, cwd=PORT):
    p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if p.returncode:
        fail('this command failed:\n  %s\n%s' % (' '.join(cmd), p.stdout[-4000:]))
    return p.stdout


def parallel(jobs):
    """Runs the commands at once (one per core) and stops at the first failure."""
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        for f in [ex.submit(run, *j) for j in jobs]:
            f.result()


def newer(target, sources):
    if not os.path.exists(target):
        return True
    t = os.path.getmtime(target)
    return any(os.path.exists(s) and os.path.getmtime(s) > t for s in sources)


def build_id():
    """The lobby's build key: SHA-1 of the runtime sources and the rollback table, with line endings
    made LF so every checkout of the same sources gives the same key."""
    m1 = os.path.join(PORT, 'm1')
    files = sorted(f for f in os.listdir(m1) if f.endswith('.c')) + sorted(f for f in os.listdir(m1) if f.endswith('.h')) \
        + sorted(f for f in os.listdir(m1) if f.endswith('.S'))
    h = hashlib.sha1()
    for p in [os.path.join(m1, f) for f in files] + [os.path.join(PORT, 'sh4c', 'sh4rt.h'), os.path.join(SC, 'ggpo', 'savestate_ranges.tsv')]:
        h.update(open(p, 'rb').read().replace(b'\r\n', b'\n'))
    return h.hexdigest()[:12]


def step_extract(gdi):
    boot = os.path.join(SC, 'extracted', '1ST_READ.BIN')
    if gdi:
        say('[1/5] Extracting the boot files from %s' % gdi)
        if not os.path.exists(gdi):
            fail('the disc dump %s does not exist' % gdi)
        out = run([sys.executable, '-I', os.path.join(SC, 'tools', 'extract_boot.py'), gdi], cwd=SC)
        say('  ' + out.strip().replace('\n', '\n  '))
    elif os.path.exists(boot):
        say('[1/5] Boot files already extracted')
    else:
        fail('no disc dump yet: pass --gdi path/to/soulcalibur.gdi (your own dump of the USA disc)')


def step_translate():
    sweep = os.path.join(PORT, 'build', 'sweep')
    inputs = [os.path.join(SC, 'extracted', '1ST_READ.BIN'), os.path.join(PORT, 'functions.json')] + \
        [os.path.join(PORT, 'sh4c', f) for f in ('sh4c.py', 'sweep.py', 'extra_entries.txt', 'hooks.txt')]
    outputs = [os.path.join(sweep, 'all_%d.c' % k) for k in range(8)] + [os.path.join(sweep, 'dispatch.c')]
    if not any(newer(o, inputs) for o in outputs):
        say('[2/5] Game code already translated')
        return
    say('[2/5] Translating the game code to C (a few minutes)...')
    t = time.time()
    run([sys.executable, '-I', os.path.join(PORT, 'sh4c', 'sweep.py'), sweep, '8'])
    say('  done in %d s' % (time.time() - t))


def step_ggpo(llvm):
    lib = os.path.join(OUT, 'ggpo', 'libggpo.a')
    src = os.path.join(SC, 'third_party', 'ggpo')
    cpp = []
    for sub in ('src/lib/ggpo', 'src/lib/ggpo/backends', 'src/lib/ggpo/network'):
        d = os.path.join(src, sub)
        cpp += [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith('.cpp') and 'platform_linux' not in f]
    if not newer(lib, cpp):
        say('[3/5] GGPO already built')
        return
    say('[3/5] Building GGPO (rollback netcode)...')
    os.makedirs(os.path.dirname(lib), exist_ok=True)
    cxx = os.path.join(llvm, 'bin', 'x86_64-w64-mingw32-clang++')
    objs = [os.path.join(OUT, 'ggpo', os.path.basename(f)[:-4] + '.o') for f in cpp]
    parallel([([cxx, '-O2', '-w', '-std=c++14', '-D_WINDOWS', '-I', 'src/include', '-I', 'src/lib/ggpo', '-c', '-o', o, f], src)
              for f, o in zip(cpp, objs)])
    if os.path.exists(lib):
        os.remove(lib)
    run([os.path.join(llvm, 'bin', 'llvm-ar'), 'rcs', lib] + objs, cwd=src)


def step_game(llvm, sdl, fsca_dir, font_zip):
    say('[4/5] Compiling and linking Voldorium...')
    obj = os.path.join(OUT, 'obj')
    os.makedirs(obj, exist_ok=True)
    os.makedirs(os.path.join(PORT, 'build', 'm1'), exist_ok=True)
    cc = os.path.join(llvm, 'bin', 'x86_64-w64-mingw32-clang')
    cf = ['-O1', '-ffp-contract=off', '-fno-strict-aliasing', '-I', 'sh4c', '-DSH4_TAG_WRITES']
    jobs = []
    # translated code: only what changed (it is large)
    sweep = os.path.join(PORT, 'build', 'sweep')
    rt = os.path.join(PORT, 'sh4c', 'sh4rt.h')
    for f in ['all_%d.c' % k for k in range(8)] + ['dispatch.c']:
        o = os.path.join(obj, f[:-2] + '.o')
        if newer(o, [os.path.join(sweep, f), rt]):
            jobs.append(([cc] + cf + ['-w', '-c', '-o', o, os.path.join(sweep, f)],))
    o = os.path.join(obj, 'fsca.o')
    if not os.path.exists(o):
        jobs.append(([cc] + cf + ['-w', '-I', fsca_dir, '-c', '-o', o, 'sh4c/fsca.c'],))
    bid = build_id()
    for name in GAME_SOURCES:
        opt = ['-O2'] if name == 'tex' else []   # texture upscaling (drawing only, not game state)
        jobs.append(([cc] + cf + opt + ['-Wall', '-I', os.path.join(sdl, 'include'), '-I', os.path.join(SC, 'third_party', 'ggpo', 'src', 'include'),
                      '-I', 'm1', '-DSC_BUILD_ID="%s"' % bid, '-c', '-o', os.path.join(obj, 'm1_%s.o' % name), 'm1/%s.c' % name],))
    jobs.append(([cc, '-c', '-o', os.path.join(obj, 'sjlj_win64.o'), 'm1/sjlj_win64.S'],))
    jobs.append(([os.path.join(llvm, 'bin', 'x86_64-w64-mingw32-windres'), '-I', 'm1', '-O', 'coff', '-o',
                  os.path.join(obj, 'voldorium_res.o'), 'm1/voldorium.rc'],))
    t = time.time()
    parallel(jobs)
    objs = sorted(os.path.join(obj, f) for f in os.listdir(obj) if f.endswith('.o'))
    libs = [os.path.join(OUT, 'ggpo', 'libggpo.a'), '-L', os.path.join(sdl, 'lib'), '-lSDL2', '-lopengl32', '-lwinmm', '-lws2_32',
            '-lcomdlg32', '-lgdi32', '-static-libstdc++', '-static-libgcc', '-Wl,--stack,67108864']   # 64 MB main stack
    run([cc + '++', '-o', os.path.join(OUT, 'm1.exe')] + objs + libs)
    run([cc + '++', '-mwindows', '-o', os.path.join(OUT, 'Voldorium.exe')] + objs + libs)   # no console window
    shutil.copy2(os.path.join(sdl, 'bin', 'SDL2.dll'), OUT)
    if font_zip and not os.path.exists(os.path.join(OUT, 'biosfont.bin')):
        zipfile.ZipFile(font_zip).extract('biosfont.bin', OUT)
    say('  built Voldorium.exe (build %s) in %d s' % (bid, time.time() - t))


def step_player(player, gdi):
    say('[5/5] Assembling the player folder %s' % player)
    os.makedirs(os.path.join(player, 'art16x9'), exist_ok=True)
    for f in ('Voldorium.exe', 'm1.exe', 'SDL2.dll', 'biosfont.bin'):
        if os.path.exists(os.path.join(OUT, f)):
            shutil.copy2(os.path.join(OUT, f), player)
    shutil.copy2(os.path.join(SC, 'ggpo', 'savestate_ranges.tsv'), player)
    refs = os.path.join(PORT, 'art16x9', 'refs.txt')   # made from your own dump, not in git
    if os.path.exists(refs):
        shutil.copy2(refs, os.path.join(player, 'art16x9'))
    old = os.path.join(player, 'Soulcalibur.exe')   # before the rename
    if os.path.exists(old):
        os.remove(old)
    if os.path.exists(os.path.join(player, 'sc-port.ini')) and not os.path.exists(os.path.join(player, 'voldorium.ini')):
        os.rename(os.path.join(player, 'sc-port.ini'), os.path.join(player, 'voldorium.ini'))
    ini = os.path.join(player, 'voldorium.ini')
    if not os.path.exists(ini):
        disc = os.path.abspath(gdi) if gdi else os.path.join(SC, 'disc', 'gdi', 'soulcalibur.gdi')
        text = open(os.path.join(PORT, 'm1', 'player', 'voldorium.ini'), encoding='utf-8').read().replace('{disc}', disc)
        open(ini, 'w', encoding='utf-8', newline='\r\n').write(text)
    readme = open(os.path.join(PORT, 'm1', 'player', 'README.txt'), encoding='utf-8').read()
    open(os.path.join(player, 'README.txt'), 'w', encoding='utf-8', newline='\r\n').write(readme)


def main():
    ap = argparse.ArgumentParser(description='Builds Voldorium from your own disc dump.')
    ap.add_argument('--gdi', help='your SoulCalibur (USA) disc dump, the .gdi file')
    ap.add_argument('--download', action='store_true', help='fetch missing tools into toolchain/')
    ap.add_argument('--player-dir', default=os.path.join(PORT, 'build', 'player'), help='where the player folder goes')
    ap.add_argument('--only', choices=['ggpo', 'game', 'player'], help='one step (for the build_*.sh wrappers)')
    a = ap.parse_args()
    if sys.platform != 'win32':
        fail('this builds the Windows version; run it on Windows')
    t = time.time()
    llvm, sdl, fsca_dir, font = (None,) * 4 if a.only == 'player' else tool_dirs(a.download)
    if a.only == 'ggpo':
        step_ggpo(llvm)
    elif a.only == 'game':
        step_game(llvm, sdl, fsca_dir, font)
    elif a.only == 'player':
        step_player(os.path.abspath(a.player_dir), a.gdi)
    else:
        step_extract(a.gdi)
        step_translate()
        step_ggpo(llvm)
        step_game(llvm, sdl, fsca_dir, font)
        step_player(os.path.abspath(a.player_dir), a.gdi)
        say('\nDone in %d min %d s. Voldorium is in %s' % ((time.time() - t) // 60, (time.time() - t) % 60, os.path.abspath(a.player_dir)))


if __name__ == '__main__':
    main()
