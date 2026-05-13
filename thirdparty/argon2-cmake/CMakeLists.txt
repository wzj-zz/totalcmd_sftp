cmake_minimum_required(VERSION 3.10)
project(argon2 LANGUAGES C)

# Sources live in the sibling submodule directory.
set(ARGON2_DIR "${CMAKE_CURRENT_LIST_DIR}/../argon2")

# Reference compress-function implementation (ref.c + blamka-round-ref.h).
# Portable C, no SSE intrinsics. Argon2 is used by the plugin only for
# PPK v3 KDF — one-shot at .ppk load — so performance of opt.c (SSE) is
# not needed and ref.c keeps x86/x64 builds identical.
#
# Excludes bench.c / genkat.c / run.c / test.c — those are upstream
# executables (CLI tool / KAT generator / benchmark / unit test), not
# part of the library.
add_library(argon2 STATIC
    "${ARGON2_DIR}/src/argon2.c"
    "${ARGON2_DIR}/src/core.c"
    "${ARGON2_DIR}/src/encoding.c"
    "${ARGON2_DIR}/src/ref.c"
    "${ARGON2_DIR}/src/thread.c"
    "${ARGON2_DIR}/src/blake2/blake2b.c"
)

# Public API header in include/, internal headers (core.h, encoding.h,
# thread.h, blake2/*.h) referenced by sources live in src/.
target_include_directories(argon2 PUBLIC
    "${ARGON2_DIR}/include"
    "${ARGON2_DIR}/src"
)
