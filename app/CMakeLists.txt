option (WITH_CLI_CLIENT "Build CLI client" ON)
option (WITH_WEB_SERVER "Build web server" ON)

if (WITH_CLI_CLIENT)
    add_subdirectory(cli)
endif ()

if (WITH_WEB_SERVER)
    add_subdirectory(web)
endif ()
