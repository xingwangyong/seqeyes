# Generates version_autogen.cpp at build time from Git metadata.
# Inputs:
#   - OUT: path to write source to (e.g. <build>/generated/version_autogen.cpp)
#   - SRC_DIR: repository root for git commands

if(NOT DEFINED OUT)
  message(FATAL_ERROR "GenVersion.cmake: OUT not specified")
endif()
if(NOT DEFINED SRC_DIR)
  message(FATAL_ERROR "GenVersion.cmake: SRC_DIR not specified")
endif()

# Default values
set(HASH "unknown")
set(DATE "unknown")

# Find git
find_package(Git QUIET)
if(Git_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C ${SRC_DIR} rev-parse --short=10 HEAD
    OUTPUT_VARIABLE HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C ${SRC_DIR} log -1 --format=%cd --date=format:%Y%m%d
    OUTPUT_VARIABLE DATE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()

set(CONTENT
"// Auto-generated at build time.
// seqeyesGitDate() is the commit date (YYYYMMDD) for seqeyesGitHash().

#include \"version_info.h\"

QString seqeyesGitHash()
{
    return QStringLiteral(\"${HASH}\");
}

QString seqeyesGitDate()
{
    return QStringLiteral(\"${DATE}\");
}
")

if(EXISTS ${OUT})
  file(READ ${OUT} OLD_CONTENT)
else()
  set(OLD_CONTENT "")
endif()

if(NOT OLD_CONTENT STREQUAL CONTENT)
  file(WRITE ${OUT} "${CONTENT}")
endif()
