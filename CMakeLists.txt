cmake_minimum_required (VERSION 2.8)
include (CheckLibraryExists)
include (CheckSymbolExists)
include (ExternalProject)
project (nrsc5 C)

option (USE_COLOR "Colorize log output")
option (USE_NEON "Use NEON instructions")
option (USE_SSE "Use SSE3 instructions")
option (USE_FAAD2 "AAC decoding with FAAD2" ON)
option (WITH_CLI_CLIENT "Build CLI client" ON)
option (WITH_WEB_SERVER "Build web server" ON)

find_program (AUTOCONF autoconf)
if (NOT AUTOCONF)
    message (FATAL_ERROR "Missing autoconf. Install autoconf package and try again.")
endif ()

find_program (AUTOMAKE automake)
if (NOT AUTOMAKE)
    message (FATAL_ERROR "Missing automake. Install autoconf package and try again.")
endif ()

find_program (LIBTOOLIZE NAMES libtoolize glibtoolize)
if (NOT LIBTOOLIZE)
    message (FATAL_ERROR "Missing libtoolize. Install libtool package and try again.")
endif ()

find_program (PATCH patch)
if (NOT PATCH)
    message (FATAL_ERROR "Missing patch. Install patch package and try again.")
endif()

find_library (FFTW3F_LIBRARY fftw3f)
find_package (SoapySDR REQUIRED)

if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm.*")
    if (USE_NEON)
        set (CMAKE_C_FLAGS "-mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4")
        add_definitions (-DHAVE_NEON)
    endif()
endif()

if (CMAKE_SYSTEM_PROCESSOR MATCHES "(i[456]|x)86.*")
    if (USE_SSE)
        set (CMAKE_C_FLAGS "-msse2 -msse3 -mssse3")
        add_definitions (-DHAVE_SSE2 -DHAVE_SSE3)
    endif()
endif()

set (CMAKE_REQUIRED_FLAGS --std=gnu11)
check_symbol_exists (strndup string.h HAVE_STRNDUP)
check_symbol_exists (CMPLXF complex.h HAVE_CMPLXF)
check_symbol_exists (_Imaginary_I complex.h HAVE_IMAGINARY_I)
check_symbol_exists (_Complex_I complex.h HAVE_COMPLEX_I)

if (USE_FAAD2)
    set (FAAD2_PREFIX "${CMAKE_BINARY_DIR}/faad2-prefix")
    ExternalProject_Add (
        faad2_external
        GIT_REPOSITORY "https://github.com/dsvensson/faad2.git"
        GIT_TAG b7aa099fd3220b71180ed2b0bc19dc6209a1b418
        PREFIX ${FAAD2_PREFIX}

        UPDATE_COMMAND ""
        PATCH_COMMAND patch -p1 -Ni "${CMAKE_SOURCE_DIR}/support/faad2-hdc-support.patch" || exit 0
        COMMAND sh ./bootstrap

        CONFIGURE_COMMAND ${FAAD2_PREFIX}/src/faad2_external/configure --prefix=${FAAD2_PREFIX} --with-hdc "CFLAGS=-O3 ${CMAKE_C_FLAGS}"

        BUILD_COMMAND make
    )

    add_library (faad2 STATIC IMPORTED)
    set_property (TARGET faad2 PROPERTY IMPORTED_LOCATION "${FAAD2_PREFIX}/lib/libfaad.a")
    add_dependencies (faad2 faad2_external)
    include_directories ("${FAAD2_PREFIX}/include")

    set (FAAD2_LIBRARY faad2)
    add_definitions (-DHAVE_FAAD2)
endif()

if (WITH_CLI_CLIENT)
    find_library (AO_LIBRARY ao)
endif()

if (WITH_WEB_SERVER)
    set (LWS_PREFIX "${CMAKE_BINARY_DIR}/lws-prefix")
    ExternalProject_Add (
        lws_external
        GIT_REPOSITORY "https://github.com/warmcat/libwebsockets.git"
        GIT_TAG 714ba5c9df85a730a6a7e28977e3a4783bddbb91
        PREFIX ${LWS_PREFIX}
        UPDATE_COMMAND ""
        CMAKE_ARGS "-DCMAKE_INSTALL_PREFIX=${LWS_PREFIX}" "-DLWS_WITH_SSL=0" "-DLWS_WITHOUT_CLIENT=1"
        INSTALL_DIR ${LWS_PREFIX}
    )
    add_library (lws STATIC IMPORTED)
    if (WIN32)
        set_property (TARGET lws PROPERTY IMPORTED_LOCATION "${LWS_PREFIX}/lib/libwebsockets_static.a")
        set (LWS_LIBRARY lws ws2_32)
    else()
        set_property (TARGET lws PROPERTY IMPORTED_LOCATION "${LWS_PREFIX}/lib/libwebsockets.a")
        set (LWS_LIBRARY lws z)
    endif()
    add_dependencies (lws lws_external)
    include_directories ("${LWS_PREFIX}/include")

    set (CJSON_PREFIX "${CMAKE_BINARY_DIR}/cjson-prefix")
    ExternalProject_Add (
        cjson_external
        GIT_REPOSITORY "https://github.com/DaveGamble/cJSON.git"
        GIT_TAG v1.7.3
        PREFIX ${CJSON_PREFIX}
        UPDATE_COMMAND ""
        CMAKE_ARGS "-DCMAKE_INSTALL_PREFIX=${CJSON_PREFIX}" "-DENABLE_CJSON_TEST=OFF" "-DBUILD_SHARED_AND_STATIC_LIBS=ON"
    )
    add_library (cjson STATIC IMPORTED)
    set_property (TARGET cjson PROPERTY IMPORTED_LOCATION "${CJSON_PREFIX}/lib/libcjson.a")
    add_dependencies (cjson cjson_external)
    include_directories ("${CJSON_PREFIX}/include")
    set (CJSON_LIBRARY cjson)

    set (OGG_PREFIX "${CMAKE_BINARY_DIR}/ogg-prefix")
    ExternalProject_Add (
        ogg_external
        GIT_REPOSITORY "https://github.com/xiph/ogg.git"
        GIT_TAG 572618e3f50f214a875d72b631115b67450fa939
        PREFIX ${OGG_PREFIX}
        UPDATE_COMMAND ""
        CMAKE_ARGS "-DCMAKE_INSTALL_PREFIX=${OGG_PREFIX}"
    )
    add_library (ogg STATIC IMPORTED)
    set_property (TARGET ogg PROPERTY IMPORTED_LOCATION "${OGG_PREFIX}/lib/libogg.a")
    add_dependencies (ogg ogg_external)
    include_directories ("${OGG_PREFIX}/include")
    set (OGG_LIBRARY ogg)

    set (VORBIS_PREFIX "${CMAKE_BINARY_DIR}/vorbis-prefix")
    ExternalProject_Add (
        vorbis_external
        GIT_REPOSITORY "https://github.com/xiph/vorbis.git"
        GIT_TAG c1c2831fc7306d5fbd7bc800324efd12b28d327f
        PREFIX ${VORBIS_PREFIX}
        UPDATE_COMMAND ""
        CMAKE_ARGS "-DCMAKE_INSTALL_PREFIX=${VORBIS_PREFIX}" "-DOGG_INCLUDE_DIRS=${OGG_PREFIX}/include" "-DOGG_LIBRARIES=${OGG_PREFIX}/lib/libogg.a"
    )
    add_library (vorbis STATIC IMPORTED)
    set_property (TARGET vorbis PROPERTY IMPORTED_LOCATION "${VORBIS_PREFIX}/lib/libvorbis.a")
    add_dependencies (vorbis_external ogg_external)
    add_dependencies (vorbis vorbis_external)
    add_library (vorbisenc STATIC IMPORTED)
    set_property (TARGET vorbisenc PROPERTY IMPORTED_LOCATION "${VORBIS_PREFIX}/lib/libvorbisenc.a")
    add_dependencies (vorbisenc vorbis_external)
    include_directories ("${VORBIS_PREFIX}/include")
    set (VORBIS_LIBRARY vorbis vorbisenc)
endif()

if(EXISTS "${CMAKE_SOURCE_DIR}/.git")
  execute_process(
    COMMAND git log -1 --format=%h
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_COMMIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
else()
  set(GIT_COMMIT_HASH "unknown")
endif()
add_definitions("-DGIT_COMMIT_HASH=\"${GIT_COMMIT_HASH}\"")

add_subdirectory (src)
