##HOW TO BUILD libportable source code?
========================================================================
##System requirements
------------------------------------------------------------------------

- C compiler 
 Microsoft Visual Studio .	
 
 or
 
 gcc, msys or msys2
 gcc for windows:
 https://sourceforge.net/projects/libportable/files/Tools/mingw8.3.1-clang-win64_32.7z
 msys msys2 project on:
 https://sourceforge.net/projects/mingw/files/MSYS/
 https://sourceforge.net/projects/msys2/

##Build!
------------------------------------------------------------------------
vc compiler  (cmd shell)

nmake -f Makefile.msvc clean
nmake -f Makefile.msvc

vs2015 or vs2017 dynamic lininking the CRT:
nmake -f Makefile.msvc clean
nmake -f Makefile.msvc MSVC_CRT=1900

enable tcmalloc memory allocator:
nmake -f Makefile.msvc TCMALLOC=1
------------------------------------------------------------------------
mingw64 compiler (msys shell)

make clean
make

enable GCC Link-time optimization(LTO): 
make LTO=1

If gcc is a cross-compiler, use the CROSS_COMPILING option:

make clean
64bits:
make CROSS_COMPILING=x86_64-w64-mingw32-
32bits:
make CROSS_COMPILING=x86_64-w64-mingw32- BITS=32
------------------------------------------------------------------------
clang compiler,If you have MSVC compiler installed
(cmd shell):

nmake -f Makefile.msvc CC=clang-cl clean
nmake -f Makefile.msvc CC=clang-cl

(msys shell):

make clean
make CC=clang DFLAGS=--target=x86_64-w64-windows-gnu    (64bits target build)
make CC=clang DFLAGS=--target=i686-w64-windows-gnu      (32bits target build)

make clean
make CC=clang DFLAGS=--target=i686-pc-windows-msvc
make CC=clang DFLAGS=--target=x86_64-pc-windows-msvc