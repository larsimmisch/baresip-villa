#
# CMakeLists.txt
#
# Copyright (C) 2010 - 2022 Alfred E. Heggestad
# Copyright (C) 2022 Sebastian Reimers
# Copyright (C) 2023 Christian Spielberger
# Copyright (C) 2023 Lars Immisch
#

cmake_minimum_required(VERSION 3.13)

project(baresip_villa VERSION 1.0.0)


list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)

##############################################################################
#
# Module/Package Includes
#

include(GNUInstallDirs)
include(CheckIncludeFile)
find_package(RE REQUIRED)
find_package(BARESIP REQUIRED)

##############################################################################
#
# Compile options
#

if(WIN32)
  option(STATIC "Build static" ON)
else()
  option(STATIC "Build static" OFF)
endif()


set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options("/W3")
else()
  add_compile_options(
    -Wall
    -Wextra
  )

  set(c_flags
    -pedantic
    -Wcast-align
    -Wbad-function-cast
    -Wmissing-declarations
    -Wmissing-prototypes
    -Wnested-externs
    -Wno-strict-aliasing
    -Wold-style-definition
    -Wshadow -Waggregate-return
    -Wstrict-prototypes
    -Wuninitialized
    -Wvla
  )

  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
      list(APPEND c_flags -Watomic-implicit-seq-cst -Wshorten-64-to-32)
  endif()

  add_compile_options(
    "$<$<COMPILE_LANGUAGE:C>:${c_flags}>"
  )
endif()



find_package(re CONFIG REQUIRED HINTS re/cmake)

list(APPEND RE_DEFINITIONS
  -DVERSION="${PROJECT_VERSION_FULL}"
  -DVER_MAJOR=${PROJECT_VERSION_MAJOR}
  -DVER_MINOR=${PROJECT_VERSION_MINOR}
  -DVER_PATCH=${PROJECT_VERSION_PATCH}
)

add_definitions(${RE_DEFINITIONS})

include_directories(
  include
  src
  .
  ${RE_INCLUDE_DIRS}
  ${BARESIP_INCLUDE_DIRS}
  ${OPENSSL_INCLUDE_DIR}
)

if(MOD_PATH)
  add_definitions(-DMOD_PATH="${MOD_PATH}")
elseif(CMAKE_INSTALL_FULL_LIBDIR)
  add_definitions(-DMOD_PATH="${CMAKE_INSTALL_FULL_LIBDIR}/baresip/modules")
endif()

if(SHARE_PATH)
  add_definitions(-DSHARE_PATH="${SHARE_PATH}")
else()
  add_definitions(-DSHARE_PATH="${CMAKE_INSTALL_FULL_DATADIR}/baresip")
endif()

if(DEFAULT_CAFILE)
  add_definitions(-DDEFAULT_CAFILE="${DEFAULT_CAFILE}")
endif()

if(DEFAULT_AUDIO_DEVICE)
  add_definitions(-DDEFAULT_AUDIO_DEVICE="${DEFAULT_AUDIO_DEVICE}")
endif()

if(STATIC)
  add_definitions(-DSTATIC)
endif()

link_libraries(${RE_LIBRARIES} ${BARESIP_LIBRARIES})

set(SOURCES src/villa.cpp
            src/villa_module.c
            baresip/modules/ctrl_tcp/netstring/netstring.c
            baresip/modules/ctrl_tcp/tcp_netstring.c)

if(STATIC)
  add_library(${PROJECT_NAME} OBJECT ${SOURCES})
else()
  add_library(${PROJECT_NAME} MODULE ${SOURCES})
endif()

##############################################################################
#
# Install section
#


if(NOT STATIC)
  install(TARGETS ${PROJECT_NAME}
    LIBRARY
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/baresip/modules
      COMPONENT Applications
  )
endif()