if(NOT EXISTS ${CMAKE_SOURCE_DIR}/src/version.h)
    file(TOUCH ${CMAKE_SOURCE_DIR}/src/version.h)
endif()

list(APPEND CMAKE_MODULE_PATH
    ${CMAKE_SOURCE_DIR}/CMakeModules)
include(GetGitRevisionDescription)

# Hardcoded version
set(GIT_VERSION "1.0")

message(NOTICE ${GIT_VERSION})

if("${GIT_VERSION}" MATCHES "GIT-NOTFOUND")
    return()
endif()

if(GIT_VERSION)
    string(REPLACE "-NOTFOUND" "" GIT_VERSION ${GIT_VERSION})
    file(READ ${CMAKE_SOURCE_DIR}/src/version.h VERSION_H)
    string(REGEX MATCH "#define VERSION \"(.+)\"" VERSION_H "${VERSION_H}")

    if(NOT GIT_VERSION STREQUAL VERSION_H)
        file(WRITE ${CMAKE_SOURCE_DIR}/src/version.h
                "// NOLINT(cata-header-guard)\n#define VERSION \"${GIT_VERSION}\"\n")
    endif()
endif()

# get_git_head_revision() does not work with worktrees in Windows
execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    OUTPUT_VARIABLE _sha1
    OUTPUT_STRIP_TRAILING_WHITESPACE)
string(TIMESTAMP _timestamp %Y-%m-%d-%H%M)
file(WRITE ${CMAKE_SOURCE_DIR}/VERSION.txt "\
build type: Release\n\
build number: ${_timestamp}\n\
commit sha: ${_sha1}\n\
commit url: https://github.com/Cataclysm-TLG/Cataclysm-TLG/commit/${_sha1}"
)
