cmake_minimum_required(VERSION 3.20)

# Defaults to CMAKE_SOURCE_DIR for in-tree builds; Android sets it explicitly.
if(NOT DEFINED CATA_REPO_ROOT)
    set(CATA_REPO_ROOT "${CMAKE_SOURCE_DIR}")
endif()

# Don't clang-tidy third party code
set(CMAKE_CXX_CLANG_TIDY "")

# Root third-party library which will export all individual ones.
add_library(
        third-party
        INTERFACE)

set(THIRD_PARTY_SOURCE_DIR ${CMAKE_SOURCE_DIR}/src/third-party)

# Flatbuffers
file(GLOB FLATBUFFERS_HEADERS flatbuffers/**/*.h)
file(GLOB FLATBUFFERS_SOURCES flatbuffers/*.cpp)

add_library(
        flatbuffers
        STATIC
        ${FLATBUFFERS_HEADERS}
        ${FLATBUFFERS_SOURCES})

target_include_directories(
        flatbuffers
        SYSTEM
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})

# For cata_flatbuffers_assert.h
target_include_directories(
        flatbuffers
        PUBLIC
        ${CATA_REPO_ROOT}/src)

target_link_libraries(
        third-party
        INTERFACE
        flatbuffers
        )
        
file(GLOB GHC_HEADERS ${THIRD_PARTY_SOURCE_DIR}/ghc/*.hpp)

add_library(
        ghc-filesystem
        INTERFACE)

target_sources(
        ghc-filesystem
        INTERFACE
        ${GHC_HEADERS})

target_include_directories(
        ghc-filesystem
        SYSTEM
        INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(
        third-party
        INTERFACE
        ghc-filesystem
        )


# We use snmalloc in header only mode
file(GLOB SNMALLOC_HEADERS snmalloc/**/*)

add_library(
        snmalloc
        INTERFACE)

target_include_directories(
        snmalloc
        SYSTEM
        INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(
        third-party
        INTERFACE
        snmalloc
        )

# Zstandard
file(GLOB ZSTD_HEADERS zstd/**/*.h)
file(GLOB ZSTD_SOURCES zstd/common/*.c zstd/compress/*.c zstd/decompress/*.c)

add_library(
        zstd
        STATIC
        ${ZSTD_HEADERS}
        ${ZSTD_SOURCES})

target_include_directories(
        zstd
        SYSTEM
        PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(
        zstd
        PUBLIC
        ZSTD_STATIC_LINKING_ONLY
        ZSTD_DISABLE_ASM
        )

target_link_libraries(
        third-party
        INTERFACE
        flatbuffers
        ghc-filesystem
        zstd)
