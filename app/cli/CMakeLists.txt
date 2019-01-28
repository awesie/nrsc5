find_library (AO_LIBRARY ao)

add_compile_options (--std=gnu11 -O3 -Wall)
include_directories ("${CMAKE_SOURCE_DIR}/include")
if (USE_COLOR)
    add_definitions (-DUSE_COLOR)
endif ()

add_executable (
    app-cli
    main.c
    log.c
)
set_property (TARGET app-cli PROPERTY OUTPUT_NAME nrsc5)
target_link_libraries (
    app-cli
    nrsc5
    ${AO_LIBRARY}
    ${THREAD_LIBRARY}
)
install (
    TARGETS app-cli
    RUNTIME DESTINATION bin
)
