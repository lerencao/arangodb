# - Find BFD (bfd.h, config.h, libbfd.so, libbfd.lib, libbfd.dll)
# This module defines
#  BFD_INCLUDE_DIR, directory containing headers
#  BFD_LIBRARY_DIR, directory containing bfd libraries
#  BFD_SHARED_LIBS, path to libbfd*.so/libbfd*.lib
#  BFD_STATIC_LIBS, path to libbfd*.a/libbfd*.lib
#  BFD_SHARED_LIB_RESOURCES, shared libraries required to use BFD, i.e. libbfd*.so/libbfd*.dll
#  BFD_FOUND, whether bfd has been found

if ("${BFD_ROOT}" STREQUAL "")
  set(BFD_ROOT "$ENV{BFD_ROOT}")
  if (NOT "${BFD_ROOT}" STREQUAL "")
    string(REPLACE "\"" "" BFD_ROOT ${BFD_ROOT})
  endif()
endif()

if ("${BFD_ROOT_SUFFIX}" STREQUAL "")
  set(BFD_ROOT_SUFFIX "$ENV{BFD_ROOT_SUFFIX}")
  if (NOT "${BFD_ROOT_SUFFIX}" STREQUAL "")
    string(REPLACE "\"" "" BFD_ROOT_SUFFIX ${BFD_ROOT_SUFFIX})
  endif()
endif()

set(BFD_SEARCH_HEADER_PATHS
  ${BFD_ROOT}/bfd
  ${BFD_ROOT}/include
)

set(BFD_SEARCH_LIB_PATH
  ${BFD_ROOT}/lib
  ${BFD_ROOT}/bfd/.libs
  ${BFD_ROOT}/libiberty
  ${BFD_ROOT}/zlib
)

if(NOT MSVC)
  set(UNIX_DEFAULT_INCLUDE "/usr/include")
endif()
find_path(BFD_INCLUDE_DIR_ANSIDECL
  ansidecl.h
  PATHS ${BFD_SEARCH_HEADER_PATHS} ${UNIX_DEFAULT_INCLUDE}
  NO_DEFAULT_PATH # make sure we don't accidentally pick up a different version
)
find_path(BFD_INCLUDE_DIR_BFD
  bfd.h
  PATHS ${BFD_SEARCH_HEADER_PATHS} ${UNIX_DEFAULT_INCLUDE}
  NO_DEFAULT_PATH # make sure we don't accidentally pick up a different version
)

include(Utils)
if(NOT MSVC)
  set(UNIX_DEFAULT_LIB "/usr/lib")
endif()


# set options for: shared
if (MSVC)
  set(BFD_LIBRARY_PREFIX "")
  set(BFD_LIBRARY_SUFFIX ".lib")
else()
  set(BFD_LIBRARY_PREFIX "lib")
  set(BFD_LIBRARY_SUFFIX ".so")
endif()
set_find_library_options("${BFD_LIBRARY_PREFIX}" "${BFD_LIBRARY_SUFFIX}")

# find library
find_library(BFD_SHARED_LIB
  NAMES bfd
  PATHS ${BFD_SEARCH_LIB_PATH} ${UNIX_DEFAULT_LIB}
  PATH_SUFFIXES ${BFD_ROOT_SUFFIX}
  NO_DEFAULT_PATH
)

# restore initial options
restore_find_library_options()


# set options for: static
if (MSVC)
  set(BFD_LIBRARY_PREFIX "")
  set(BFD_LIBRARY_SUFFIX ".lib")
else()
  set(BFD_LIBRARY_PREFIX "lib")
  set(BFD_LIBRARY_SUFFIX ".a")
endif()
set_find_library_options("${BFD_LIBRARY_PREFIX}" "${BFD_LIBRARY_SUFFIX}")

# find library
find_library(BFD_STATIC_LIB
  NAMES bfd
  PATHS ${BFD_SEARCH_LIB_PATH} ${UNIX_DEFAULT_LIB}
  PATH_SUFFIXES ${BFD_ROOT_SUFFIX}
  NO_DEFAULT_PATH
)
find_library(BFD_STATIC_LIB_IBERTY
  NAMES iberty
  PATHS ${BFD_SEARCH_LIB_PATH} ${UNIX_DEFAULT_LIB}
  PATH_SUFFIXES ${BFD_ROOT_SUFFIX}
  NO_DEFAULT_PATH
)
find_library(BFD_STATIC_LIB_Z
  NAMES z
  PATHS ${BFD_SEARCH_LIB_PATH} ${UNIX_DEFAULT_LIB}
  PATH_SUFFIXES ${BFD_ROOT_SUFFIX}
  NO_DEFAULT_PATH
)

# restore initial options
restore_find_library_options()


if (BFD_INCLUDE_DIR_ANSIDECL AND BFD_INCLUDE_DIR_BFD AND BFD_SHARED_LIB AND BFD_STATIC_LIB AND BFD_STATIC_LIB_IBERTY AND BFD_STATIC_LIB_Z)
  set(BFD_FOUND TRUE)
  list(APPEND BFD_INCLUDE_DIR ${BFD_INCLUDE_DIR_ANSIDECL} ${BFD_INCLUDE_DIR_BFD})
  list(APPEND BFD_SHARED_LIBS ${BFD_SHARED_LIB})
  list(APPEND BFD_STATIC_LIBS ${BFD_STATIC_LIB} ${BFD_STATIC_LIB_IBERTY} ${BFD_STATIC_LIB_Z})
  set(BFD_LIBRARY_DIR
    "${BFD_SEARCH_LIB_PATH}"
    CACHE PATH
    "Directory containing bfd libraries"
    FORCE
  )

  # build a list of shared libraries (staticRT)
  foreach(ELEMENT ${BFD_SHARED_LIBS})
    get_filename_component(ELEMENT_FILENAME ${ELEMENT} NAME)
    string(REGEX MATCH "^(.*)\\.(lib|so)$" ELEMENT_MATCHES ${ELEMENT_FILENAME})

    if(NOT ELEMENT_MATCHES)
      continue()
    endif()

    get_filename_component(ELEMENT_DIRECTORY ${ELEMENT} DIRECTORY)
    file(GLOB ELEMENT_LIB
      "${ELEMENT_DIRECTORY}/${CMAKE_MATCH_1}.so"
      "${ELEMENT_DIRECTORY}/lib${CMAKE_MATCH_1}.so"
      "${ELEMENT_DIRECTORY}/${CMAKE_MATCH_1}-[0-9\.]*.so"
      "${ELEMENT_DIRECTORY}/lib${CMAKE_MATCH_1}-[0-9\.]*.so"
    )

    if(ELEMENT_LIB)
      list(APPEND BFD_SHARED_LIB_RESOURCES ${ELEMENT_LIB})
    endif()
  endforeach()

  #TODO add shared support
  add_library(bfd_static IMPORTED)
  add_target_include_directories(bfd_static PUBLIC ${BFD_INCLUDE_DIR})
  add_target_link_libraries(bfd_static PUBLIC ${BFD_STATIC_LIBS})

else ()
  set(BFD_FOUND FALSE)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(BFD
  DEFAULT_MSG
  BFD_INCLUDE_DIR
  BFD_SHARED_LIBS
  BFD_STATIC_LIBS
  BFD_INCLUDE_DIR_ANSIDECL
  BFD_INCLUDE_DIR_BFD
  BFD_SHARED_LIB
  BFD_STATIC_LIB
  BFD_STATIC_LIB_IBERTY
  BFD_STATIC_LIB_Z
)
message("BFD_INCLUDE_DIR: " ${BFD_INCLUDE_DIR})
message("BFD_LIBRARY_DIR: " ${BFD_LIBRARY_DIR})
message("BFD_SHARED_LIBS: " ${BFD_SHARED_LIBS})
message("BFD_STATIC_LIBS: " ${BFD_STATIC_LIBS})
message("BFD_SHARED_LIB_RESOURCES: " ${BFD_SHARED_LIB_RESOURCES})

mark_as_advanced(
  BFD_INCLUDE_DIR
  BFD_LIBRARY_DIR
  BFD_SHARED_LIBS
  BFD_STATIC_LIBS
  BFD_INCLUDE_DIR_ANSIDECL
  BFD_INCLUDE_DIR_BFD
  BFD_SHARED_LIB
  BFD_STATIC_LIB
  BFD_STATIC_LIB_IBERTY
  BFD_STATIC_LIB_Z
)