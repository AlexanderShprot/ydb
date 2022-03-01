
# This file was gererated by the build system used internally in the Yandex monorepo.
# Only simple modifications are allowed (adding source-files to targets, adding simple properties
# like target_include_directories). These modifications will be ported to original
# ya.make files by maintainers. Any complex modifications which can't be ported back to the
# original buildsystem will not be accepted.


set(COMMON_PROTOC_FLAGS -I=${CMAKE_SOURCE_DIR} -I=${CMAKE_BINARY_DIR})
set(YASM_FLAGS -f elf64 -D UNIX -D _x86_64_ -D_YASM_ -g dwarf2)
set(BISON_FLAGS -v)
set(RAGEL_FLAGS -L -I ${CMAKE_SOURCE_DIR}/)
