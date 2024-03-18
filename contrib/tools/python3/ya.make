# Generated by devtools/yamaker from nixpkgs 22.11.

LIBRARY()

VERSION(3.12.2)

ORIGINAL_SOURCE(https://github.com/python/cpython/archive/v3.12.2.tar.gz)

LICENSE(Python-2.0)

PEERDIR(
    contrib/libs/expat
    contrib/libs/libbz2
    contrib/libs/libc_compat
    contrib/libs/lzma
    contrib/libs/openssl
    contrib/libs/zlib
    contrib/restricted/libffi
    contrib/tools/python3/Modules/_decimal
    library/cpp/sanitizer/include
)

ADDINCL(
    contrib/libs/expat
    contrib/libs/libbz2
    contrib/restricted/libffi/include
    contrib/tools/python3/Include
    contrib/tools/python3/Include/internal
    contrib/tools/python3/Modules
    contrib/tools/python3/Modules/_hacl/include
    contrib/tools/python3/PC
)

NO_COMPILER_WARNINGS()

NO_UTIL()

CFLAGS(
    -DPy_BUILD_CORE
    -DPy_BUILD_CORE_BUILTIN
)

IF (CLANG_CL)
    CFLAGS(
        -Wno-invalid-token-paste
    )
ENDIF()

IF (OS_DARWIN)
    LDFLAGS(
        -framework CoreFoundation
        -framework SystemConfiguration
    )
ELSEIF (OS_WINDOWS)
    CFLAGS(
        -DPY3_DLLNAME=L\"python3\"
    )

    LDFLAGS(
        Mincore.lib
        Shlwapi.lib
        Winmm.lib
    )

    # DISABLE(MSVC_INLINE_OPTIMIZED)
ENDIF()

SRCS(
    Modules/_abc.c
    Modules/_asynciomodule.c
    Modules/_bisectmodule.c
    Modules/_blake2/blake2b_impl.c
    Modules/_blake2/blake2module.c
    Modules/_blake2/blake2s_impl.c
    Modules/_bz2module.c
    Modules/_codecsmodule.c
    Modules/_collectionsmodule.c
    Modules/_contextvarsmodule.c
    Modules/_csv.c
    Modules/_ctypes/_ctypes.c
    Modules/_ctypes/callbacks.c
    Modules/_ctypes/callproc.c
    Modules/_ctypes/cfield.c
    Modules/_ctypes/stgdict.c
    Modules/_datetimemodule.c
    Modules/_elementtree.c
    Modules/_functoolsmodule.c
    Modules/_hacl/Hacl_Hash_MD5.c
    Modules/_hacl/Hacl_Hash_SHA1.c
    Modules/_hacl/Hacl_Hash_SHA2.c
    Modules/_hacl/Hacl_Hash_SHA3.c
    Modules/_hashopenssl.c
    Modules/_heapqmodule.c
    Modules/_io/_iomodule.c
    Modules/_io/bufferedio.c
    Modules/_io/bytesio.c
    Modules/_io/fileio.c
    Modules/_io/iobase.c
    Modules/_io/stringio.c
    Modules/_io/textio.c
    Modules/_io/winconsoleio.c
    Modules/_json.c
    Modules/_localemodule.c
    Modules/_lsprof.c
    Modules/_lzmamodule.c
    Modules/_multiprocessing/multiprocessing.c
    Modules/_multiprocessing/posixshmem.c
    Modules/_multiprocessing/semaphore.c
    Modules/_opcode.c
    Modules/_operator.c
    Modules/_pickle.c
    Modules/_queuemodule.c
    Modules/_randommodule.c
    Modules/_sre/sre.c
    Modules/_ssl.c
    Modules/_stat.c
    Modules/_statisticsmodule.c
    Modules/_struct.c
    Modules/_threadmodule.c
    Modules/_tracemalloc.c
    Modules/_typingmodule.c
    Modules/_weakref.c
    Modules/_xxinterpchannelsmodule.c
    Modules/_xxsubinterpretersmodule.c
    Modules/_xxtestfuzz/_xxtestfuzz.c
    Modules/_xxtestfuzz/fuzzer.c
    Modules/_zoneinfo.c
    Modules/arraymodule.c
    Modules/atexitmodule.c
    Modules/audioop.c
    Modules/binascii.c
    Modules/cjkcodecs/_codecs_cn.c
    Modules/cjkcodecs/_codecs_hk.c
    Modules/cjkcodecs/_codecs_iso2022.c
    Modules/cjkcodecs/_codecs_jp.c
    Modules/cjkcodecs/_codecs_kr.c
    Modules/cjkcodecs/_codecs_tw.c
    Modules/cjkcodecs/multibytecodec.c
    Modules/cmathmodule.c
    Modules/config.c
    Modules/errnomodule.c
    Modules/faulthandler.c
    Modules/gcmodule.c
    Modules/getbuildinfo.c
    Modules/getpath.c
    Modules/itertoolsmodule.c
    Modules/main.c
    Modules/mathmodule.c
    Modules/md5module.c
    Modules/mmapmodule.c
    Modules/posixmodule.c
    Modules/pyexpat.c
    Modules/rotatingtree.c
    Modules/selectmodule.c
    Modules/sha1module.c
    Modules/sha2module.c
    Modules/sha3module.c
    Modules/signalmodule.c
    Modules/socketmodule.c
    Modules/symtablemodule.c
    Modules/timemodule.c
    Modules/unicodedata.c
    Modules/zlibmodule.c
    Objects/abstract.c
    Objects/boolobject.c
    Objects/bytearrayobject.c
    Objects/bytes_methods.c
    Objects/bytesobject.c
    Objects/call.c
    Objects/capsule.c
    Objects/cellobject.c
    Objects/classobject.c
    Objects/codeobject.c
    Objects/complexobject.c
    Objects/descrobject.c
    Objects/dictobject.c
    Objects/enumobject.c
    Objects/exceptions.c
    Objects/fileobject.c
    Objects/floatobject.c
    Objects/frameobject.c
    Objects/funcobject.c
    Objects/genericaliasobject.c
    Objects/genobject.c
    Objects/interpreteridobject.c
    Objects/iterobject.c
    Objects/listobject.c
    Objects/longobject.c
    Objects/memoryobject.c
    Objects/methodobject.c
    Objects/moduleobject.c
    Objects/namespaceobject.c
    Objects/object.c
    Objects/obmalloc.c
    Objects/odictobject.c
    Objects/picklebufobject.c
    Objects/rangeobject.c
    Objects/setobject.c
    Objects/sliceobject.c
    Objects/structseq.c
    Objects/tupleobject.c
    Objects/typeobject.c
    Objects/typevarobject.c
    Objects/unicodectype.c
    Objects/unicodeobject.c
    Objects/unionobject.c
    Objects/weakrefobject.c
    Parser/action_helpers.c
    Parser/myreadline.c
    Parser/parser.c
    Parser/peg_api.c
    Parser/pegen.c
    Parser/pegen_errors.c
    Parser/string_parser.c
    Parser/token.c
    Parser/tokenizer.c
    Python/Python-ast.c
    Python/Python-tokenize.c
    Python/_warnings.c
    Python/asdl.c
    Python/assemble.c
    Python/ast.c
    Python/ast_opt.c
    Python/ast_unparse.c
    Python/bltinmodule.c
    Python/bootstrap_hash.c
    Python/ceval.c
    Python/ceval_gil.c
    Python/codecs.c
    Python/compile.c
    Python/context.c
    Python/deepfreeze/deepfreeze.c
    Python/dtoa.c
    Python/dynamic_annotations.c
    Python/errors.c
    Python/fileutils.c
    Python/flowgraph.c
    Python/formatter_unicode.c
    Python/frame.c
    Python/frozen.c
    Python/future.c
    Python/getargs.c
    Python/getcompiler.c
    Python/getcopyright.c
    Python/getopt.c
    Python/getplatform.c
    Python/getversion.c
    Python/hamt.c
    Python/hashtable.c
    Python/import.c
    Python/importdl.c
    Python/initconfig.c
    Python/instrumentation.c
    Python/intrinsics.c
    Python/legacy_tracing.c
    Python/marshal.c
    Python/modsupport.c
    Python/mysnprintf.c
    Python/mystrtoul.c
    Python/pathconfig.c
    Python/perf_trampoline.c
    Python/preconfig.c
    Python/pyarena.c
    Python/pyctype.c
    Python/pyfpe.c
    Python/pyhash.c
    Python/pylifecycle.c
    Python/pymath.c
    Python/pystate.c
    Python/pystrcmp.c
    Python/pystrhex.c
    Python/pystrtod.c
    Python/pythonrun.c
    Python/pytime.c
    Python/specialize.c
    Python/structmember.c
    Python/suggestions.c
    Python/symtable.c
    Python/sysmodule.c
    Python/thread.c
    Python/traceback.c
    Python/tracemalloc.c
)

IF (OS_WINDOWS)
    SRCS(
        Modules/_winapi.c
        Modules/overlapped.c
        PC/WinMain.c
        PC/invalid_parameter_handler.c
        PC/msvcrtmodule.c
        PC/winreg.c
        PC/winsound.c
        Python/dynload_win.c
    )
ELSE()
    SRCS(
        Modules/_cryptmodule.c
        Modules/_posixsubprocess.c
        Modules/fcntlmodule.c
        Modules/grpmodule.c
        Modules/pwdmodule.c
        Modules/resource.c
        Modules/syslogmodule.c
        Modules/termios.c
        Python/dynload_shlib.c
    )
ENDIF()

IF (OS_DARWIN)
    SRCS(
        Modules/_scproxy.c
    )
ELSEIF (OS_LINUX)
    IF (NOT MUSL)
        EXTRALIBS(crypt)
    ENDIF()

    SRCS(
        Modules/spwdmodule.c
        Python/asm_trampoline.S
    )
ENDIF()

SUPPRESSIONS(
    tsan.supp
)

END()

RECURSE(
    Lib
    Modules/_decimal
    Modules/_sqlite
    bin
)
