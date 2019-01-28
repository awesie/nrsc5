set (LWS_PREFIX "${CMAKE_BINARY_DIR}/lws-prefix")
ExternalProject_Add (
    lws_external
    GIT_REPOSITORY "https://github.com/warmcat/libwebsockets.git"
    GIT_TAG 714ba5c9df85a730a6a7e28977e3a4783bddbb91
    PREFIX ${LWS_PREFIX}
    UPDATE_COMMAND ""
    CMAKE_ARGS "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_INSTALL_PREFIX=${LWS_PREFIX}" "-DLWS_WITH_SSL=0" "-DLWS_WITHOUT_CLIENT=1"
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
    CMAKE_ARGS "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_INSTALL_PREFIX=${CJSON_PREFIX}" "-DENABLE_CJSON_TEST=OFF" "-DBUILD_SHARED_AND_STATIC_LIBS=ON"
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
    CMAKE_ARGS "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_INSTALL_PREFIX=${OGG_PREFIX}"
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
    CMAKE_ARGS "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_INSTALL_PREFIX=${VORBIS_PREFIX}" "-DOGG_INCLUDE_DIRS=${OGG_PREFIX}/include" "-DOGG_LIBRARIES=${OGG_PREFIX}/lib/libogg.a"
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

add_compile_options (--std=gnu11 -O3 -Wall)
include_directories ("${CMAKE_SOURCE_DIR}/include")
if (USE_COLOR)
    add_definitions (-DUSE_COLOR)
endif ()

add_executable (
    app-web
    main.c
)
set_property (TARGET app-web PROPERTY OUTPUT_NAME nrsc5-web)
target_link_libraries (
    app-web
    nrsc5
    ${CJSON_LIBRARY}
    ${LWS_LIBRARY}
    ${VORBIS_LIBRARY}
    ${OGG_LIBRARY}
    ${THREAD_LIBRARY}
)
install (
    TARGETS app-web
    RUNTIME DESTINATION bin
)
