cmake_minimum_required(VERSION 3.20)
project(Alforno VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---- Backend selection (Pasta or Basta) ---------------------------------

option(ALF_USE_BASTA "Build against Basta (binary blob support) instead of Pasta" OFF)

include(FetchContent)

if(ALF_USE_BASTA)
    FetchContent_Declare(
        basta
        GIT_REPOSITORY https://github.com/IridiumFX/basta.git
        GIT_TAG        main
    )
    set(BASTA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(basta)
    set(ALF_BACKEND_LIB basta)
else()
    FetchContent_Declare(
        pasta
        GIT_REPOSITORY https://github.com/IridiumFX/Pasta.git
        GIT_TAG        main
    )
    set(PASTA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(pasta)
    set(ALF_BACKEND_LIB pasta)
endif()

# ---- Library --------------------------------------------------------

if(NOT DEFINED BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS ON)
endif()

add_library(alforno
    src/main/c/alforno.c
    src/main/c/alforno_param.c
    src/main/c/alforno_merge.c
    src/main/c/alforno_link.c
)

target_include_directories(alforno
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/main/h>
            $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/main/c
)

target_link_libraries(alforno PUBLIC ${ALF_BACKEND_LIB})

if(ALF_USE_BASTA)
    target_compile_definitions(alforno PUBLIC ALF_USE_BASTA)
endif()

if(BUILD_SHARED_LIBS)
    target_compile_definitions(alforno PRIVATE ALF_BUILDING)
    set_target_properties(alforno PROPERTIES
        VERSION   ${PROJECT_VERSION}
        SOVERSION 0
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
else()
    target_compile_definitions(alforno PUBLIC ALF_STATIC)
    set_target_properties(alforno PROPERTIES
        VERSION ${PROJECT_VERSION}
    )
endif()

if(WIN32)
    set_target_properties(alforno PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib
    )
endif()

# ---- Tests ----------------------------------------------------------

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(ALF_BUILD_TESTS "Build test executable" ON)
if(ALF_BUILD_TESTS)
    add_executable(alforno_test src/test/c/alforno_test.c)
    target_link_libraries(alforno_test PRIVATE alforno ${ALF_BACKEND_LIB})
    target_include_directories(alforno_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/main/c
    )
    target_compile_definitions(alforno_test PRIVATE
        ALF_TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/src/test/resources"
    )
endif()

# ---- Install --------------------------------------------------------

include(GNUInstallDirs)
install(TARGETS alforno
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES src/main/h/alforno.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
