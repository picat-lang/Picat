cmake_minimum_required(VERSION 3.27)
project(picat
        LANGUAGES C CXX
        VERSION 7.3
)

include(GNUInstallDirs)

if(DEFINED OPENBSD_LOCALBASE)
    include_directories(${OPENBSD_LOCALBASE}/include)
    link_directories(${OPENBSD_LOCALBASE}/lib)
endif()

set(CMAKE_MODULE_PATH
        ${CMAKE_CURRENT_LIST_DIR}/cmake
        ${CMAKE_MODULE_PATH}
)

if(${CMAKE_SYSTEM_NAME} STREQUAL "OpenBSD")
        set(SYSTEM_NAME "OPENBSD")
elseif(${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
        set(SYSTEM_NAME "LINUX")
else()
        message(FATAL_ERROR "Unsupported system: ${CMAKE_SYSTEM_NAME}")
endif()

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(64BITS M64BITS)
    message(STATUS "Set 64bit")
endif ()

add_compile_definitions(unix POSIX ${SYSTEM_NAME} GC PICAT SAT ${64BITS})
add_compile_options(-O3 -fno-strict-aliasing -finline-functions -fomit-frame-pointer -Wno-error=unused-label)

add_subdirectory(emu)
