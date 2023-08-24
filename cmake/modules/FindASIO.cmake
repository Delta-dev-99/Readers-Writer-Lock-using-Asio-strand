
# Find ASIO header-only library and
# create an interface libray to link it

set(ASIO_FOUND FALSE)

include(FindPackageHandleStandardArgs)

if (TARGET ASIO::ASIO)
    set(ASIO_FOUND TRUE)
else ()
    set(SEARCHPATHS
		/opt/local
		# /sw # NOTE: Do we need to look in this folder?
		/usr
		/usr/local
	)
    find_path(ASIO_INCLUDE_DIR
        NAMES
            asio.hpp
        PATH_SUFFIXES
            include
        PATHS
            ${SEARCHPATHS}
    )

    if (ASIO_INCLUDE_DIR)
        set(ASIO_FOUND TRUE)
        add_library(ASIO::ASIO INTERFACE IMPORTED GLOBAL)
        target_compile_definitions(ASIO::ASIO INTERFACE "ASIO_STANDALONE")
        target_include_directories(ASIO::ASIO INTERFACE ${ASIO_INCLUDE_DIR})

        message(STATUS "Found ASIO headers: " ${ASIO_INCLUDE_DIR})
        # ASIO requires thread library
        find_package(Threads)
        if (Threads_FOUND)
            target_link_libraries(ASIO::ASIO INTERFACE Threads::Threads)
        else()
            if (ASIO_FIND_REQUIRED)
                message(WARNING "ASIO requires thread support, but it wasn't found.")
            else ()
                message(STATUS "ASIO requires thread support, but it wasn't found.")
            endif ()
        endif ()
    endif ()
endif()


if (ASIO_FOUND)
    set(ASIO_INCLUDE_DIRS ${ASIO_INCLUDE_DIR})
else()
    set(ASIO_INCLUDE_DIRS )
endif()


FIND_PACKAGE_HANDLE_STANDARD_ARGS(ASIO DEFAULT_MSG ASIO_INCLUDE_DIRS)

mark_as_advanced(ASIO_INCLUDE_DIRS)
