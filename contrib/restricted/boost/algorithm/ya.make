# Generated by devtools/yamaker from nixpkgs 22.11.

LIBRARY()

LICENSE(BSL-1.0)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

VERSION(1.86.0)

ORIGINAL_SOURCE(https://github.com/boostorg/algorithm/archive/boost-1.86.0.tar.gz)

PEERDIR(
    contrib/restricted/boost/array
    contrib/restricted/boost/assert
    contrib/restricted/boost/bind
    contrib/restricted/boost/concept_check
    contrib/restricted/boost/config
    contrib/restricted/boost/core
    contrib/restricted/boost/exception
    contrib/restricted/boost/function
    contrib/restricted/boost/iterator
    contrib/restricted/boost/mpl
    contrib/restricted/boost/range
    contrib/restricted/boost/regex
    contrib/restricted/boost/static_assert
    contrib/restricted/boost/throw_exception
    contrib/restricted/boost/tuple
    contrib/restricted/boost/type_traits
    contrib/restricted/boost/unordered
)

ADDINCL(
    GLOBAL contrib/restricted/boost/algorithm/include
)

NO_COMPILER_WARNINGS()

NO_UTIL()

END()
