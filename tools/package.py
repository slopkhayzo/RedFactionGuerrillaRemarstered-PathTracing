"""Make the release packages: zips to extract into the game folder, with the release settings (tools/release).

python tools/package.py [--config RelWithDebInfo]

Build first, through the shared queue (tools/build.ps1 builds both architectures). Writes into dist/:
  rfgrt-<version>/                             the package's files (extract into the folder with rfg.exe)
  rfgrt-<version>-with-asi-loader.zip          all of it
  rfgrt-<version>-plugin-only.zip              without Ultimate ASI Loader, for games that already have one
  rfgrt-<version>-symbols.zip                  the PDBs and the binaries they belong to, for crash reports

Today's game is 32-bit: the package holds the 32-bit plugin and the 64-bit ray tracing process it starts. The 64-bit
plugin (build-x64, the core in-process) waits for a 64-bit rfg.exe.
"""
import argparse
import hashlib
import os
import re
import shutil
import sys
import zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
UAL_SHA256 = 'd5a059aa467a7a7127c8f6169f79fa63ff0f55986ee9eb2fd9a281bebf2aa2e6'  # Ultimate ASI Loader 9.7.4

CRASHDUMPS_README = """While this folder exists, Ultimate ASI Loader writes a crash dump here when the game crashes (about 60 MB
each). Delete the folder to turn that off.
"""


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def zip_dir(src, dst, skip=()):
    with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for base, dirs, files in os.walk(src):
            dirs.sort()
            for name in sorted(files):
                full = os.path.join(base, name)
                rel = os.path.relpath(full, src).replace(os.sep, '/')
                if rel not in skip:
                    z.write(full, rel)


def fail(message):
    print(message)
    sys.exit(2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--config', default='RelWithDebInfo')
    args = ap.parse_args()

    with open(os.path.join(ROOT, 'CMakeLists.txt'), encoding='utf-8') as f:
        version = re.search(r'project\(rfgrt VERSION ([0-9.]+)', f.read()).group(1)
    name = f'rfgrt-{version}'
    dist = os.path.join(ROOT, 'dist')
    out = os.path.join(dist, name)

    x86 = os.path.join(ROOT, 'build-x86', args.config)
    x64 = os.path.join(ROOT, 'build-x64', args.config)
    files = {
        'asi': os.path.join(x86, 'rfgrt.asi'),
        'asi_pdb': os.path.join(x86, 'rfgrt.pdb'),
        'host': os.path.join(x64, 'rfgrt_host.exe'),
        'host_pdb': os.path.join(x64, 'rfgrt_host.pdb'),
        'ual': os.path.join(ROOT, 'third_party', 'ultimate-asi-loader', 'dinput8.dll'),
        'ini': os.path.join(ROOT, 'tools', 'release', 'rfgrt.ini'),
        'readme': os.path.join(ROOT, 'tools', 'release', 'README.txt'),
        'license': os.path.join(ROOT, 'LICENSE'),
    }
    for path in files.values():
        if not os.path.exists(path):
            fail(f'missing: {path} (build first, or write the release files)')
    if sha256(files['ual']) != UAL_SHA256:
        fail('dinput8.dll is not the known Ultimate ASI Loader 9.7.4')
    # A package never ships binaries older than their sources (as tools/run_game.ps1 checks before a test).
    for binary, sources in ((files['asi'], ('src/rfgrt', 'src/shared')), (files['host'], ('host', 'src/rtcore', 'src/shared'))):
        newest = max(os.path.getmtime(os.path.join(base, n)) for src in sources
                     for base, _, names in os.walk(os.path.join(ROOT, src)) for n in names)
        if os.path.getmtime(binary) < newest:
            fail(f'{binary} is older than its sources: build first (tools/build.ps1)')

    if os.path.exists(out):
        shutil.rmtree(out)
    scripts = os.path.join(out, 'scripts')
    for d in (os.path.join(scripts, 'rfgrt'), os.path.join(out, 'rfgrt_licenses'), os.path.join(out, 'CrashDumps')):
        os.makedirs(d)
    shutil.copy2(files['ual'], out)
    shutil.copy2(files['asi'], scripts)
    shutil.copy2(files['host'], os.path.join(scripts, 'rfgrt'))
    # DLSS Ray Reconstruction's runtime, beside the core, when the build has it (tools/fetch_dlss.ps1), with NVIDIA's licence
    dlss_runtime = os.path.join(x64, 'nvngx_dlssd.dll')
    dlss_license = os.path.join(ROOT, 'third_party', 'dlss', 'LICENSE.txt')
    with_dlss = os.path.exists(dlss_runtime) and os.path.exists(dlss_license)
    if with_dlss:
        shutil.copy2(dlss_runtime, os.path.join(scripts, 'rfgrt'))
    else:
        print('note: no nvngx_dlssd.dll in the build: the package has no DLSS Ray Reconstruction')
    # DLSS Frame Generation: rfgrt_present.exe (the plugin's presenter, 64-bit: Streamline is) with Streamline's runtime
    # beside it, when the build has them (tools/fetch_streamline.ps1), with NVIDIA's licences
    present = os.path.join(x64, 'rfgrt_present.exe')
    sl_runtimes = ('sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll', 'sl.reflex.dll', 'sl.pcl.dll', 'nvngx_dlssg.dll')
    sl_dir = os.path.join(ROOT, 'third_party', 'streamline')
    with_fg = os.path.exists(present) and all(os.path.exists(os.path.join(x64, n)) for n in sl_runtimes) and         os.path.exists(os.path.join(sl_dir, 'license.txt'))
    if with_fg:
        newest = max(os.path.getmtime(os.path.join(base, n)) for src in ('src/present_host',)
                     for base, _, names in os.walk(os.path.join(ROOT, src)) for n in names)
        if os.path.getmtime(present) < newest:
            fail(f'{present} is older than its sources: build first (tools/build.ps1)')
        shutil.copy2(present, os.path.join(scripts, 'rfgrt'))
        for n in sl_runtimes:
            shutil.copy2(os.path.join(x64, n), os.path.join(scripts, 'rfgrt'))
    else:
        print('note: no rfgrt_present.exe with Streamline in the build: the package has no DLSS Frame Generation')
    with open(files['ini'], encoding='ascii') as f:
        ini = f.read()
    with open(os.path.join(scripts, 'rfgrt.ini'), 'w', encoding='ascii', newline='\r\n') as f:
        f.write(ini)
    with open(files['readme'], encoding='utf-8') as f:
        readme = f.read().replace('@VERSION@', version)
    # the title's underline as long as the title with the version in it
    lines = readme.split('\n')
    if len(lines) > 1 and lines[1] and set(lines[1]) == {'='}:
        lines[1] = '=' * len(lines[0])
        readme = '\n'.join(lines)
    with open(os.path.join(out, 'rfgrt_README.txt'), 'w', encoding='utf-8', newline='\r\n') as f:
        f.write(readme)
    with open(os.path.join(out, 'CrashDumps', 'README.txt'), 'w', encoding='utf-8', newline='\r\n') as f:
        f.write(CRASHDUMPS_README)
    licenses = {
        'rfgrt-MIT.txt': 'LICENSE',
        'Ultimate-ASI-Loader-MIT.txt': os.path.join('third_party', 'ultimate-asi-loader', 'LICENSE.txt'),
        'MinHook-BSD-2-Clause.txt': os.path.join('third_party', 'minhook', 'LICENSE.txt'),
    }
    if with_dlss:
        licenses['NVIDIA-DLSS-LICENSE.txt'] = os.path.join('third_party', 'dlss', 'LICENSE.txt')
    if with_fg:
        licenses['NVIDIA-Streamline-LICENSE.txt'] = os.path.join('third_party', 'streamline', 'license.txt')
        licenses['NVIDIA-Streamline-3rd-party-licenses.md'] = os.path.join('third_party', 'streamline', '3rd-party-licenses.md')
    for dst, src in licenses.items():
        shutil.copy2(os.path.join(ROOT, src), os.path.join(out, 'rfgrt_licenses', dst))

    # Two archives: everything, and the same without the loader for games that already have one.
    full = os.path.join(dist, f'{name}-with-asi-loader.zip')
    plugin_only = os.path.join(dist, f'{name}-plugin-only.zip')
    zip_dir(out, full)
    zip_dir(out, plugin_only, skip={'dinput8.dll', 'rfgrt_licenses/Ultimate-ASI-Loader-MIT.txt'})

    symbols_dir = os.path.join(dist, f'{name}-symbols')
    if os.path.exists(symbols_dir):
        shutil.rmtree(symbols_dir)
    os.makedirs(symbols_dir)
    for key in ('asi', 'asi_pdb', 'host', 'host_pdb'):
        shutil.copy2(files[key], symbols_dir)
    symbols = os.path.join(dist, f'{name}-symbols.zip')
    zip_dir(symbols_dir, symbols)
    shutil.rmtree(symbols_dir)

    print(f'{name} (plugin-only: all but dinput8.dll and its license):')
    for base, _, names in os.walk(out):
        for n in sorted(names):
            p = os.path.join(base, n)
            print(f'  {os.path.relpath(p, out):<45} {os.path.getsize(p):>12,} bytes')
    for package in (full, plugin_only):
        print(f'{os.path.basename(package)}: {os.path.getsize(package):,} bytes, SHA-256 {sha256(package)}')
    print(f'symbols: {symbols} ({os.path.getsize(symbols):,} bytes)')


if __name__ == '__main__':
    main()
