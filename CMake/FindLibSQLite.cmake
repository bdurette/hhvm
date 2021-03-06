# Finds libsqlite3.
#
# This module defines:
# LIBSQLITE3_INCLUDE_DIR
# LIBSQLITE3_LIBRARY
#

find_package(PkgConfig)
pkg_check_modules(PC_SQLITE3 QUIET sqlite3)

if (PC_SQLITE3_FOUND AND PC_SQLITE3_VERSION VERSION_EQUAL "3.8.5")
    set(LIBSQLITE3_FOUND 0)
    set(LIBSQLITE3_INCLUDE_DIR "LIBSQLITE3_INCLUDE_DIR-NOTFOUND")
    set(LIBSQLITE3_LIBRARY "LIBSQLITE3_LIBRARY-NOTFOUND")
else()
    find_path(LIBSQLITE3_INCLUDE_DIR
        NAMES sqlite3.h
        HINTS ${PC_SQLITE3_INCLUDE_DIRS})

    find_library(LIBSQLITE3_LIBRARY
        NAMES sqlite3)
endif (PC_SQLITE3_FOUND AND PC_SQLITE3_VERSION VERSION_EQUAL "3.8.5")

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(
    LIBSQLITE3 DEFAULT_MSG
    LIBSQLITE3_LIBRARY LIBSQLITE3_INCLUDE_DIR)

if (NOT LIBSQLITE3_FOUND)
  message(STATUS "Using third-party bundled libsqlite3")
else()
  message(STATUS "Found libsqlite3: ${LIBSQLITE3_LIBRARY}")
endif (NOT LIBSQLITE3_FOUND)
