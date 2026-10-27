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
OUT = os.path.join(LOCAL, 'sc-m1')          # objects and executables (outside the repository; --out moves them)

# pinned downloads: name -> (url, sha256, folder inside toolchain/ once unpacked). The SHA-256s are
# the ones GitHub publishes for the release files, and for Flycast's two files those of its commit
FLYCAST_COMMIT = '5aa091fde632fb332c8d8c34e280d62dc951954c'
PINNED = {
    'llvm-mingw': ('https://github.com/mstorsjo/llvm-mingw/releases/download/20260922/llvm-mingw-20260922-ucrt-x86_64.zip',
                   'e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666', 'llvm-mingw-20260922-ucrt-x86_64'),
    'sdl2': ('https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.zip',
             'f15cff5fca62ec9381a016ef1d42a95c638cd72d2f226ba5781c76fe43dbd1ac', 'SDL2-2.32.10'),
    'fsca-table.h': ('https://raw.githubusercontent.com/flyinghead/flycast/%s/core/hw/sh4/fsca-table.h' % FLYCAST_COMMIT,
                     '2ab87d6df67546c8a27febafb2c2e520fd1266b91633e4edfa8ec48b58218300', 'flycast/core/hw/sh4'),
    'biosfont.bin.zip': ('https://raw.githubusercontent.com/flyinghead/flycast/%s/fonts/biosfont.bin.zip' % FLYCAST_COMMIT,
                         'e996bc0f9854fbe7ad057948c8cda6ff9ef9b4b6d0347c92f1ecad71a92214ce', 'flycast/fonts'),
}

GAME_SOURCES = ['m1', 'hw', 'hle', 'threads', 'maple', 'ta', 'tex', 'render', 'state', 'netplay', 'aica', 'arm7',
                'config', 'online', 'artdump', 'artswap', 'png', 'module']
# the translator: with the same disc dump (checked by SHA-1) and compiler, the same game module
TRANSLATOR = ['sh4c.py', 'sweep.py', 'extra_entries.txt', 'hooks.txt', 'module.c', 'fsca.c']


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
    if digest != sha:
        fail('%s does not match its pinned SHA-256 (got %s). The download was damaged or changed; nothing was used.' % (name, digest))
    say('  %s: %.1f MB, SHA-256 ok' % (name, len(data) / 1e6))
    return data


def tool_dirs(allow_download, need_sdl=True):
    """(llvm-mingw, SDL2 x86_64 folder, folder holding fsca-table.h, biosfont.bin.zip or None).
    need_sdl False: the game module alone (no SDL2 needed or fetched)."""
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
    missing = [n for n, v in (('llvm-mingw', llvm), ('sdl2', sdl), ('fsca-table.h', fsca), ('biosfont.bin.zip', font))
               if not v and (need_sdl or n != 'sdl2')]
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
        return tool_dirs(False, need_sdl)
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
    """The lobby's build key: SHA-1 of the runtime sources, the translator and the rollback table,
    with line endings made LF so every checkout of the same sources gives the same key. Peers with
    the same key run the same executable and the same game module (built from the same disc)."""
    m1 = os.path.join(PORT, 'm1')
    files = sorted(f for f in os.listdir(m1) if f.endswith('.c')) + sorted(f for f in os.listdir(m1) if f.endswith('.h')) \
        + sorted(f for f in os.listdir(m1) if f.endswith('.S'))
    h = hashlib.sha1()
    for p in [os.path.join(m1, f) for f in files] + [os.path.join(PORT, 'sh4c', f) for f in ['sh4rt.h'] + TRANSLATOR] + \
            [os.path.join(PORT, 'functions.json'), os.path.join(SC, 'ggpo', 'savestate_ranges.tsv')]:
        h.update(open(p, 'rb').read().replace(b'\r\n', b'\n'))
    return h.hexdigest()[:12]


def step_extract(gdi, label='[1/5]'):
    boot = os.path.join(SC, 'extracted', '1ST_READ.BIN')
    if gdi:
        say('%s Extracting the boot files from %s' % (label, gdi))
        if not os.path.exists(gdi):
            fail('the disc dump %s does not exist' % gdi)
        out = run([sys.executable, '-I', os.path.join(SC, 'tools', 'extract_boot.py'), gdi], cwd=SC)
        say('  ' + out.strip().replace('\n', '\n  '))
    elif os.path.exists(boot):
        say('%s Boot files already extracted' % label)
    else:
        fail('no disc dump yet: pass --gdi path/to/soulcalibur.gdi (your own dump of the USA disc)')


def step_translate(label='[2/5]'):
    sweep = os.path.join(PORT, 'build', 'sweep')
    inputs = [os.path.join(SC, 'extracted', '1ST_READ.BIN'), os.path.join(PORT, 'functions.json')] + \
        [os.path.join(PORT, 'sh4c', f) for f in ('sh4c.py', 'sweep.py', 'extra_entries.txt', 'hooks.txt')]
    outputs = [os.path.join(sweep, 'all_%d.c' % k) for k in range(8)] + [os.path.join(sweep, 'dispatch.c')]
    if not any(newer(o, inputs) for o in outputs):
        say('%s Game code already translated' % label)
        return
    say('%s Translating the game code to C (a few minutes)...' % label)
    t = time.time()
    run([sys.executable, '-I', os.path.join(PORT, 'sh4c', 'sweep.py'), sweep, '8'])
    say('  done in %d s' % (time.time() - t))


def step_ggpo(llvm):
    lib = os.path.join(OUT, 'ggpo', 'libggpo.a')
    src = os.path.join(SC, 'third_party', 'ggpo')
    cpp = []
    for sub in ('src/lib/ggpo', 'src/lib/ggpo/backends', 'src/lib/ggpo/network'):
        d = os.path.join(src, sub)
        cpp += [sub + '/' + f for f in sorted(os.listdir(d)) if f.endswith('.cpp') and 'platform_linux' not in f]   # relative: asserts name the file
    if not newer(lib, [os.path.join(src, f) for f in cpp]):
        say('[3/5] GGPO already built')
        return
    say('[3/5] Building GGPO (rollback netcode)...')
    os.makedirs(os.path.dirname(lib), exist_ok=True)
    cxx = os.path.join(llvm, 'bin', 'x86_64-w64-mingw32-clang++')
    objs = [os.path.join(OUT, 'ggpo', os.path.basename(f)[:-4] + '.o') for f in cpp]
    parallel([([cxx, '-O2', '-w', '-std=c++14', '-D_WINDOWS', '-mno-incremental-linker-compatible', '-ffile-prefix-map=%s=.' % src, '-I', 'src/include', '-I', 'src/lib/ggpo', '-c', '-o', o, f], src)
              for f, o in zip(cpp, objs)])
    if os.path.exists(lib):
        os.remove(lib)
    run([os.path.join(llvm, 'bin', 'llvm-ar'), 'rcsD', lib] + objs, cwd=src)   # D: no time stamps


# reproducible: no time stamps in objects or executables, so the same sources build the same bytes,
# and no folder names (they would carry the user's name)
def cflags():
    return ['-O1', '-ffp-contract=off', '-fno-strict-aliasing', '-I', 'sh4c', '-DSH4_TAG_WRITES', '-mno-incremental-linker-compatible',
            '-ffile-prefix-map=%s=.' % PORT]


def translator_id():
    """Names the translator a game module comes from (module.c), checked by the executable."""
    return hashlib.sha1(b''.join(open(os.path.join(PORT, 'sh4c', f), 'rb').read().replace(b'\r\n', b'\n')
                                 for f in TRANSLATOR)).hexdigest()[:12]


def step_module(llvm, fsca_dir, font_zip, out, label='[4/5]'):
    """The game module sc_game.dll: the translated code, the fsca table (Flycast's, fetched at build
    time) and module.c. Also the BIOS font next to it (Flycast's, fetched the same way)."""
    say('%s Compiling the game module (a minute or two)...' % label)
    gobj = os.path.join(OUT, 'obj', 'game')
    os.makedirs(gobj, exist_ok=True)
    cc = os.path.join(llvm, 'bin', 'x86_64-w64-mingw32-clang')
    sweep = os.path.join(PORT, 'build', 'sweep')
    rt = os.path.join(PORT, 'sh4c', 'sh4rt.h')
    gf = cflags() + ['-DSH4_MODULE', '-fno-auto-import', '-w']   # its globals are its own: no .refptr indirection
    jobs = []
    for f in ['all_%d.c' % k for k in range(8)] + ['dispatch.c']:   # only what changed (it is large)
        o = os.path.join(gobj, f[:-2] + '.o')
        if newer(o, [os.path.join(sweep, f), rt]):
            jobs.append(([cc] + gf + ['-c', '-o', o, os.path.join(sweep, f)],))
    o = os.path.join(gobj, 'fsca.o')
    if newer(o, [os.path.join(PORT, 'sh4c', 'fsca.c'), rt]):
        jobs.append(([cc] + gf + ['-I', fsca_dir, '-c', '-o', o, 'sh4c/fsca.c'],))
    jobs.append(([cc] + gf + ['-DSH4_TRANSLATOR="%s"' % translator_id(), '-c', '-o', os.path.join(gobj, 'module.o'), 'sh4c/module.c'],))
    jobs.append(([cc, '-mno-incremental-linker-compatible', '-c', '-o', os.path.join(gobj, 'sjlj_win64.o'), 'm1/sjlj_win64.S'],))
    t = time.time()
    parallel(jobs)
    gobjs = [os.path.join(gobj, f) for f in ['all_%d.o' % k for k in range(8)] + ['dispatch.o', 'fsca.o', 'module.o', 'sjlj_win64.o']]
    dll = os.path.join(out, 'sc_game.dll')
    run([cc, '-shared', '-o', dll] + gobjs + ['-static-libgcc', '-Wl,--no-insert-timestamp'])
    if font_zip and not os.path.exists(os.path.join(out, 'biosfont.bin')):
        zipfile.ZipFile(font_zip).extract('biosfont.bin', out)
    say('  built sc_game.dll (SHA-256 %s) in %d s' % (hashlib.sha256(open(dll, 'rb').read()).hexdigest()[:16], time.time() - t))


def step_host(llvm, sdl):
    """Voldorium.exe and m1.exe: the runtime, renderer, sound, netplay and lobby. No game code: the
    game module (step_module) is loaded at start (m1/module.c)."""
    say('[4/5] Compiling and linking Voldorium...')
    obj = os.path.join(OUT, 'obj')
    os.makedirs(obj, exist_ok=True)
    os.makedirs(os.path.join(PORT, 'build', 'm1'), exist_ok=True)
    cc = os.path.join(llvm, 'bin', 'x86_64-w64-mingw32-clang')
    cf = cflags()
    bid = build_id()
    jobs = []
    for name in GAME_SOURCES:
        opt = ['-O2'] if name == 'tex' else []   # texture upscaling (drawing only, not game state)
        jobs.append(([cc] + cf + opt + ['-Wall', '-I', os.path.join(sdl, 'include'), '-I', os.path.join(SC, 'third_party', 'ggpo', 'src', 'include'),
                      '-I', 'm1', '-DSC_BUILD_ID="%s"' % bid, '-DSC_TRANSLATOR="%s"' % translator_id(), '-c', '-o',
                      os.path.join(obj, 'm1_%s.o' % name), 'm1/%s.c' % name],))
    jobs.append(([cc, '-mno-incremental-linker-compatible', '-c', '-o', os.path.join(obj, 'sjlj_win64.o'), 'm1/sjlj_win64.S'],))
    jobs.append(([os.path.join(llvm, 'bin', 'x86_64-w64-mingw32-windres'), '-I', 'm1', '-O', 'coff', '-o',
                  os.path.join(obj, 'voldorium_res.o'), 'm1/voldorium.rc'],))
    t = time.time()
    parallel(jobs)
    objs = [os.path.join(obj, 'm1_%s.o' % n) for n in GAME_SOURCES] + [os.path.join(obj, 'sjlj_win64.o'), os.path.join(obj, 'voldorium_res.o')]
    libs = [os.path.join(OUT, 'ggpo', 'libggpo.a'), '-L', os.path.join(sdl, 'lib'), '-lSDL2', '-lopengl32', '-lwinmm', '-lws2_32',
            '-lcomdlg32', '-lgdi32', '-static-libstdc++', '-static-libgcc', '-Wl,--stack,67108864',   # 64 MB main stack
            '-Wl,--no-insert-timestamp']
    run([cc + '++', '-o', os.path.join(OUT, 'm1.exe')] + sorted(objs) + libs)
    run([cc + '++', '-mwindows', '-o', os.path.join(OUT, 'Voldorium.exe')] + sorted(objs) + libs)   # no console window
    shutil.copy2(os.path.join(sdl, 'bin', 'SDL2.dll'), OUT)
    say('  built Voldorium.exe (build %s) in %d s' % (bid, time.time() - t))


def step_game(llvm, sdl, fsca_dir, font_zip):
    step_host(llvm, sdl)
    step_module(llvm, fsca_dir, font_zip, OUT)


# the release folder: what a player downloads. No game code and nothing made from the disc: the
# game module is built on the player's PC at the first start (m1/module.c, builder/first_run.bat).
# Flycast's two files are fetched by that build too, not shipped.
BUILDER = ['tools/build.py', 'tools/extract_boot.py', 'port/functions.json', 'port/m1/sjlj_win64.S'] + \
    ['port/sh4c/' + f for f in ['sh4rt.h'] + TRANSLATOR]


def step_release(rel):
    say('Assembling the release folder %s' % rel)
    if os.path.exists(rel):
        fail('%s already exists: give a new folder' % rel)
    os.makedirs(os.path.join(rel, 'textures'))
    for f in ('Voldorium.exe', 'm1.exe', 'SDL2.dll'):
        shutil.copy2(os.path.join(OUT, f), rel)
    shutil.copy2(os.path.join(SC, 'ggpo', 'savestate_ranges.tsv'), rel)
    for f in os.listdir(os.path.join(PORT, 'm1', 'textures')):
        shutil.copy2(os.path.join(PORT, 'm1', 'textures', f), os.path.join(rel, 'textures'))
    os.makedirs(os.path.join(rel, 'art16x9'))
    text = open(os.path.join(PORT, 'm1', 'player', 'voldorium.ini'), encoding='utf-8').read().replace('{disc}', '')
    open(os.path.join(rel, 'voldorium.ini'), 'w', encoding='utf-8', newline='\r\n').write(text)
    readme = open(os.path.join(PORT, 'm1', 'player', 'README.txt'), encoding='utf-8').read()
    open(os.path.join(rel, 'README.txt'), 'w', encoding='utf-8', newline='\r\n').write(readme)
    b = os.path.join(rel, 'builder')
    for f in BUILDER:
        os.makedirs(os.path.dirname(os.path.join(b, f)), exist_ok=True)
        shutil.copy2(os.path.join(SC, f), os.path.join(b, f))
    bat = open(os.path.join(PORT, 'm1', 'player', 'first_run.bat'), encoding='utf-8').read()
    open(os.path.join(b, 'first_run.bat'), 'w', encoding='utf-8', newline='\r\n').write(bat)
    # the check: no translated function in the executables, no piece of the game's executable anywhere
    boot = os.path.join(SC, 'extracted', '1ST_READ.BIN')
    img = open(boot, 'rb').read() if os.path.exists(boot) else b''
    samples = [img[i:i + 64] for i in range(0, len(img) - 64, 4096) if img[i:i + 64].count(0) < 32]
    for root, _, files in os.walk(rel):
        for f in files:
            data = open(os.path.join(root, f), 'rb').read()
            binary = f.lower().endswith(('.exe', '.dll'))   # the builder's own sources name them, of course
            if (binary and b'f_8c0' in data) or any(x in data for x in samples):
                fail('%s holds game code: not releasing' % os.path.join(root, f))
    say('  checked: no game code in %d files (%d samples of 1ST_READ.BIN looked for)' %
        (sum(len(f) for _, _, f in os.walk(rel)), len(samples)))


def step_player(player, gdi):
    say('[5/5] Assembling the player folder %s' % player)
    os.makedirs(os.path.join(player, 'art16x9'), exist_ok=True)
    for f in ('Voldorium.exe', 'm1.exe', 'sc_game.dll', 'SDL2.dll', 'biosfont.bin'):
        if os.path.exists(os.path.join(OUT, f)):
            shutil.copy2(os.path.join(OUT, f), player)
    shutil.copy2(os.path.join(SC, 'ggpo', 'savestate_ranges.tsv'), player)
    os.makedirs(os.path.join(player, 'textures'), exist_ok=True)   # replacement art (port/m1/tex.c)
    for f in os.listdir(os.path.join(PORT, 'm1', 'textures')):
        shutil.copy2(os.path.join(PORT, 'm1', 'textures', f), os.path.join(player, 'textures'))
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
    ap.add_argument('--only', choices=['ggpo', 'game', 'player', 'module', 'release'],
                    help='one step (for the build_*.sh wrappers); module: the game module alone, from --gdi, into --module-dir')
    ap.add_argument('--module-dir', help='where --only module puts sc_game.dll and biosfont.bin')
    ap.add_argument('--release-dir', help='a new folder for --only release (after a full build)')
    ap.add_argument('--out', help='objects and executables (default %%LOCALAPPDATA%%/sc-m1)')
    a = ap.parse_args()
    global OUT
    if a.out:
        OUT = os.path.abspath(a.out)
    if 'WindowsApps' in sys.executable:
        # the Microsoft Store Python keeps what it writes under AppData in a private copy that the
        # compiler does not see ("unable to open output file ... No such file or directory")
        say('Note: this is the Microsoft Store Python; build files go to the work folder here instead.')
        if not a.out:
            OUT = os.path.join(SC, 'work')
    if sys.platform != 'win32':
        fail('this builds the Windows version; run it on Windows')
    t = time.time()
    llvm, sdl, fsca_dir, font = (None,) * 4 if a.only in ('player', 'release') else tool_dirs(a.download, a.only != 'module')
    if a.only == 'module':   # the first run of a released Voldorium (m1/module.c): the game from the player's disc
        if not a.module_dir:
            fail('--only module needs --module-dir')
        step_extract(a.gdi, '[1/3]')
        step_translate('[2/3]')
        step_module(llvm, fsca_dir, font, os.path.abspath(a.module_dir), '[3/3]')
        say('\nDone in %d min %d s.' % ((time.time() - t) // 60, (time.time() - t) % 60))
    elif a.only == 'release':
        if not a.release_dir:
            fail('--only release needs --release-dir')
        step_release(os.path.abspath(a.release_dir))
    elif a.only == 'ggpo':
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
