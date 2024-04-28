cmake_minimum_required(VERSION 3.26)

project(nexer)

set(CMAKE_CXX_STANDARD 17)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

option(NEXER_CODE_COVERAGE "Build for code coverage" OFF)

if(NEXER_CODE_COVERAGE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")
endif()

include(ExternalProject)

ExternalProject_Add(
    llhttp
    GIT_REPOSITORY https://github.com/nodejs/llhttp
    GIT_TAG v9.2.1
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/deps/llhttp
    BINARY_DIR ${CMAKE_SOURCE_DIR}/deps/llhttp
    CONFIGURE_COMMAND ""
    BUILD_COMMAND npm install && make
    INSTALL_COMMAND ""    # Disable install step
    TEST_COMMAND ""       # Disable test step
    GIT_SHALLOW 1
    UPDATE_DISCONNECTED 1 # Do not update if it is already present
)

set(llhttp_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/deps/llhttp/build CACHE INTERNAL "")
set(llhttp_LIBRARIES ${CMAKE_SOURCE_DIR}/deps/llhttp/build/libllhttp.a CACHE INTERNAL "")

ExternalProject_Add(
    libuv
    GIT_REPOSITORY https://github.com/libuv/libuv
    GIT_TAG v1.x
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/deps/libuv
    BINARY_DIR ${CMAKE_SOURCE_DIR}/deps/libuv
    CONFIGURE_COMMAND cmake -DBUILD_TESTING=OFF .
    BUILD_COMMAND cmake --build .
    INSTALL_COMMAND ""    # Disable install step
    TEST_COMMAND ""       # Disable test step
    GIT_SHALLOW 1
    UPDATE_DISCONNECTED 1 # Do not update if it is already present
)
set(libuv_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/deps/libuv/include CACHE INTERNAL "")
set(libuv_LIBRARIES ${CMAKE_SOURCE_DIR}/deps/libuv/libuv.a CACHE INTERNAL "")

ExternalProject_Add(
    jsini
    GIT_REPOSITORY https://github.com/fangwd/jsini
    GIT_TAG master
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/deps/jsini
    BINARY_DIR ${CMAKE_SOURCE_DIR}/deps/jsini
    CONFIGURE_COMMAND cmake .
    BUILD_COMMAND cmake --build .
    INSTALL_COMMAND ""    # Disable install step
    TEST_COMMAND ""       # Disable test step
    GIT_SHALLOW 1
    UPDATE_DISCONNECTED 1 # Do not update if it is already present
)
set(jsini_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/deps/jsini/include CACHE INTERNAL "")
set(jsini_LIBRARIES ${CMAKE_SOURCE_DIR}/deps/jsini/libjsini.a CACHE INTERNAL "")

add_library(nex STATIC
    src/config.cc
    src/event_loop.cc
    src/handle.cc
    src/http_message.cc
    src/http_server.cc
    src/logger.cc
    src/nexer.cc
    src/process.cc
    src/process_manager.cc
    src/tcp_client.cc
    src/tcp_proxy.cc
    src/tcp_server.cc
    src/timer.cc
    src/url.cc
)

target_include_directories(nex
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_include_directories(nex
  PRIVATE ${llhttp_INCLUDE_DIRS}
  ${libuv_INCLUDE_DIRS}
  ${jsini_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(nex PRIVATE ${llhttp_LIBRARIES} ${libuv_LIBRARIES} ${jsini_LIBRARIES})

set(CMAKE_INSTALL_INCLUDEDIR include)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Tests

enable_testing()

list(APPEND test_sources
  test/main.cc
  test/helper.cc
  test/runner.cc
  test/test_config.cc
  test/test_process.cc
  test/test_process_manager.cc
  test/test_tcp_client.cc
  test/test_tcp_server.cc
  test/test_timer.cc
)

add_executable(run_test ${test_sources})

target_include_directories(run_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${llhttp_INCLUDE_DIRS}
  ${libuv_INCLUDE_DIRS}
  ${libjsini_INCLUDE_DIRS}
)

target_link_libraries(run_test PRIVATE nex sqlite3)

install(TARGETS nex
        EXPORT nexer-targets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin
)

# nexer

list(APPEND nexer_sources
  src/main.cc
)

add_executable(nexer ${nexer_sources})

target_include_directories(nexer PRIVATE ${llhttp_INCLUDE_DIRS} ${libuv_INCLUDE_DIRS} ${jsrw_INCLUDE_DIRS})
target_link_libraries(nexer PRIVATE nex)
