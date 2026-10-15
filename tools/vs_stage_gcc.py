#!/usr/bin/env python3
"""Stage a compile-only gcc into a release's overlay_toolchain/.

Takes an extracted w64devkit (https://github.com/skeeto/w64devkit, x64) and
copies just what compile_overlays.py needs to build overlay DLLs: the gcc
driver, cc1, as, ld, the CRT objects and import libraries it links, and the
C (not C++ or Windows SDK) headers. Writes overlay_toolchain/compiler.txt so
the runtime and tools/vs_overlay_precompile.py use it instead of TinyCC, and
the license notices the binaries need.

  python tools/vs_stage_gcc.py --devkit <w64devkit dir> \
      --toolchain <release>/overlay_toolchain --gpl3 <GPLv3 text>
"""
import argparse
import glob
import os
import shutil

TRIPLE = 'x86_64-w64-mingw32'
BIN = ['gcc.exe', 'as.exe', 'ld.exe']
LIBEXEC = ['cc1.exe', 'collect2.exe', 'lto-wrapper.exe']
GCC_LIBS = ['crtbegin.o', 'crtend.o', 'libgcc.a']  # static runtime: no libgcc_eh
# Import/runtime libraries named by gcc's default link line plus -lm.
LIBS = ['mingw32', 'mingwex', 'msvcrt', 'moldname', 'kernel32', 'user32',
        'advapi32', 'shell32', 'pthread', 'winpthread', 'm']
# C library and mingw-w64 internal headers; the Windows SDK and C++ headers
# (95 MB) are never included by generated overlay C.
C_HEADERS = '''assert complex ctype errno fenv float inttypes iso646 limits locale
math setjmp signal stdalign stdarg stdatomic stdbool stddef stdint stdio stdlib
stdnoreturn string tgmath threads time uchar wchar wctype malloc memory process
io direct fcntl search excpt vadefs crtdefs swprintf unistd strings
pthread_unistd'''.split()
HEADER_DIRS = ['sec_api', 'sys', 'psdk_inc']
# gcc's own headers: freestanding C, not the x86 intrinsics.
GCC_HEADERS = '''float.h iso646.h limits.h stdalign.h stdarg.h stdatomic.h
stdbool.h stddef.h stdfix.h stdint.h stdint-gcc.h stdnoreturn.h syslimits.h
varargs.h stdckdint.h'''.split()


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--devkit', required=True)
    ap.add_argument('--toolchain', required=True)
    ap.add_argument('--gpl3', required=True, help='GPLv3 text to ship beside gcc')
    a = ap.parse_args()

    kit = os.path.abspath(a.devkit)
    version = open(os.path.join(kit, 'VERSION.txt')).read().strip()
    gccver = os.path.basename(glob.glob(os.path.join(kit, 'lib', 'gcc', TRIPLE, '*'))[0])
    out = os.path.join(os.path.abspath(a.toolchain), 'gcc')
    if os.path.isdir(out):
        shutil.rmtree(out)

    for f in BIN:
        copy(os.path.join(kit, 'bin', f), os.path.join(out, 'bin', f))
    for f in LIBEXEC:
        copy(os.path.join(kit, 'libexec', 'gcc', TRIPLE, gccver, f),
             os.path.join(out, 'libexec', 'gcc', TRIPLE, gccver, f))
    gcclib = os.path.join('lib', 'gcc', TRIPLE, gccver)
    for f in GCC_LIBS:
        copy(os.path.join(kit, gcclib, f), os.path.join(out, gcclib, f))
    for f in GCC_HEADERS:
        src = os.path.join(kit, gcclib, 'include', f)
        if os.path.exists(src):
            copy(src, os.path.join(out, gcclib, 'include', f))
    shutil.copytree(os.path.join(kit, gcclib, 'include-fixed'),
                    os.path.join(out, gcclib, 'include-fixed'))
    for f in glob.glob(os.path.join(kit, 'lib', '*.o')):
        copy(f, os.path.join(out, 'lib', os.path.basename(f)))
    for name in LIBS:
        src = os.path.join(kit, 'lib', 'lib%s.a' % name)
        if os.path.exists(src):
            copy(src, os.path.join(out, 'lib', os.path.basename(src)))
    inc = os.path.join(kit, 'include')
    for f in os.listdir(inc):
        stem, ext = os.path.splitext(f)
        if (ext == '.h' and (stem in C_HEADERS or stem.startswith(('_', 'corecrt'))))\
                or ext == '.inl':
            copy(os.path.join(inc, f), os.path.join(out, 'include', f))
    for d in HEADER_DIRS:
        if os.path.isdir(os.path.join(inc, d)):
            shutil.copytree(os.path.join(inc, d), os.path.join(out, 'include', d))

    copy(os.path.join(kit, 'COPYING.MinGW-w64-runtime.txt'),
         os.path.join(out, 'COPYING.MinGW-w64-runtime.txt'))
    copy(a.gpl3, os.path.join(out, 'COPYING.GPLv3.txt'))
    with open(os.path.join(out, 'README.txt'), 'w', newline='\r\n') as f:
        f.write(
            'GCC %s (x86_64-w64-mingw32), GNU Binutils and mingw-w64, taken\n'
            'unmodified from w64devkit %s and reduced to what compiling\n'
            'overlay DLLs needs. Vagrant Story Recompiled uses it on your PC to\n'
            'compile the game code it reads from your own disc.\n\n'
            'GCC and Binutils are free software under the GNU General Public\n'
            'License version 3 (COPYING.GPLv3.txt). Code GCC compiles is not\n'
            'covered by the GPL (GCC Runtime Library Exception). The mingw-w64\n'
            'runtime and headers are under COPYING.MinGW-w64-runtime.txt.\n\n'
            'Complete corresponding source code for these binaries:\n'
            '  https://github.com/skeeto/w64devkit/releases/tag/v%s\n'
            '  (source.tar, which contains every upstream source archive and\n'
            '  the build recipe).\n' % (gccver, version, version))

    # Relative to the exe directory; the runtime and the precompile script
    # both resolve it there.
    with open(os.path.join(a.toolchain, 'compiler.txt'), 'w', newline='\n') as f:
        f.write('overlay_toolchain/gcc/bin/gcc.exe\n')

    size = sum(os.path.getsize(os.path.join(r, f))
               for r, _, fs in os.walk(out) for f in fs)
    print('staged gcc %s from w64devkit %s: %.1f MB -> %s' % (
        gccver, version, size / 1e6, out))


if __name__ == '__main__':
    main()
