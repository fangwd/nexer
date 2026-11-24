cmake_minimum_required(VERSION 3.26)

project(nexer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

option(NEXER_CODE_COVERAGE "Build for code coverage" OFF)
option(NEXER_ASAN "Build with AddressSanitizer" OFF)
option(NEXER_VALGRIND "Build with Valgrind-friendly flags" OFF)

include(GNUInstallDirs)
include(ExternalProject)

find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)

if(APPLE)
  option(NEXER_SIGN_FOR_LEAKS "Codesign executables with get-task-allow so macOS leaks can attach" ON)
  set(NEXER_DEBUG_ENTITLEMENTS "${CMAKE_SOURCE_DIR}/cmake/nexer-debug-entitlements.plist")
  if(NEXER_SIGN_FOR_LEAKS AND NOT EXISTS "${NEXER_DEBUG_ENTITLEMENTS}")
    message(FATAL_ERROR "Missing debug entitlements file: ${NEXER_DEBUG_ENTITLEMENTS}")
  endif()
endif()

if(NEXER_CODE_COVERAGE)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")
endif()

if(NEXER_VALGRIND)
  if(NEXER_ASAN)
    message(FATAL_ERROR "NEXER_VALGRIND and NEXER_ASAN are incompatible; disable one of them.")
  endif()
  set(NEXER_VALGRIND_CFLAGS "-O0 -g -gdwarf-4 -fno-omit-frame-pointer")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(NEXER_VALGRIND_CFLAGS "${NEXER_VALGRIND_CFLAGS} -gstrict-dwarf")
  endif()
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${NEXER_VALGRIND_CFLAGS}")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${NEXER_VALGRIND_CFLAGS}")
  set(NEXER_VALGRIND_CMAKE_ARGS
    "-DCMAKE_C_FLAGS=${NEXER_VALGRIND_CFLAGS}"
    "-DCMAKE_CXX_FLAGS=${NEXER_VALGRIND_CFLAGS}"
    "-DCMAKE_BUILD_TYPE=Debug")
endif()

if(NEXER_ASAN)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address -fsanitize=undefined")
endif()

if(APPLE)
  function(nexer_enable_debug_sign target)
    if(NEXER_SIGN_FOR_LEAKS)
      add_custom_command(
        TARGET ${target}
        POST_BUILD
        COMMAND codesign -s - --force --timestamp=none --entitlements "${NEXER_DEBUG_ENTITLEMENTS}" $<TARGET_FILE:${target}>
        COMMENT "Codesigning ${target} with debug entitlements for leaks"
        VERBATIM
      )
    endif()
  endfunction()
else()
  function(nexer_enable_debug_sign target)
  endfunction()
endif()

if(NEXER_VALGRIND)
  set(NEXER_LLHTTP_BUILD_COMMAND
    ${CMAKE_COMMAND} -E env CFLAGS=${NEXER_VALGRIND_CFLAGS} CXXFLAGS=${NEXER_VALGRIND_CFLAGS} npm install
    COMMAND ${CMAKE_COMMAND} -E env CFLAGS=${NEXER_VALGRIND_CFLAGS} CXXFLAGS=${NEXER_VALGRIND_CFLAGS} make clean
    COMMAND ${CMAKE_COMMAND} -E env CFLAGS=${NEXER_VALGRIND_CFLAGS} CXXFLAGS=${NEXER_VALGRIND_CFLAGS} make)
else()
  set(NEXER_LLHTTP_BUILD_COMMAND npm install && make)
endif()

ExternalProject_Add(
  llhttp
  GIT_REPOSITORY https://github.com/nodejs/llhttp
  GIT_TAG v9.2.1
  SOURCE_DIR ${CMAKE_SOURCE_DIR}/deps/llhttp
  BINARY_DIR ${CMAKE_SOURCE_DIR}/deps/llhttp
  CONFIGURE_COMMAND ""
  BUILD_COMMAND ${NEXER_LLHTTP_BUILD_COMMAND}
  INSTALL_COMMAND ""
  TEST_COMMAND ""
  GIT_SHALLOW 1
  UPDATE_DISCONNECTED 1
)
set(llhttp_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/deps/llhttp/build CACHE INTERNAL "")
set(llhttp_LIBRARIES ${CMAKE_SOURCE_DIR}/deps/llhttp/build/libllhttp.a CACHE INTERNAL "")

if(NEXER_VALGRIND)
  set(NEXER_LIBUV_CONFIGURE_COMMAND cmake -DBUILD_TESTING=OFF ${NEXER_VALGRIND_CMAKE_ARGS} .)
  set(NEXER_LIBUV_BUILD_COMMAND cmake --build . --clean-first)
else()
  set(NEXER_LIBUV_CONFIGURE_COMMAND cmake -DBUILD_TESTING=OFF .)
  set(NEXER_LIBUV_BUILD_COMMAND cmake --build .)
endif()

ExternalProject_Add(
  libuv
  GIT_REPOSITORY https://github.com/libuv/libuv
  GIT_TAG v1.x
  SOURCE_DIR ${CMAKE_SOURCE_DIR}/deps/libuv
  BINARY_DIR ${CMAKE_SOURCE_DIR}/deps/libuv
  CONFIGURE_COMMAND ${NEXER_LIBUV_CONFIGURE_COMMAND}
  BUILD_COMMAND ${NEXER_LIBUV_BUILD_COMMAND}
  INSTALL_COMMAND ""
  TEST_COMMAND ""
  GIT_SHALLOW 1
  UPDATE_DISCONNECTED 1
)
set(libuv_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/deps/libuv/include CACHE INTERNAL "")
set(libuv_LIBRARIES ${CMAKE_SOURCE_DIR}/deps/libuv/libuv.a CACHE INTERNAL "")

if(NEXER_VALGRIND)
  set(NEXER_JSINI_CONFIGURE_COMMAND cmake ${NEXER_VALGRIND_CMAKE_ARGS} .)
  set(NEXER_JSINI_BUILD_COMMAND cmake --build . --clean-first)
else()
  set(NEXER_JSINI_CONFIGURE_COMMAND cmake .)
  set(NEXER_JSINI_BUILD_COMMAND cmake --build .)
endif()

ExternalProject_Add(
  jsini
  GIT_REPOSITORY https://github.com/fangwd/jsini
  GIT_TAG master
  SOURCE_DIR ${CMAKE_SOURCE_DIR}/deps/jsini
  BINARY_DIR ${CMAKE_SOURCE_DIR}/deps/jsini
  CONFIGURE_COMMAND ${NEXER_JSINI_CONFIGURE_COMMAND}
  BUILD_COMMAND ${NEXER_JSINI_BUILD_COMMAND}
  INSTALL_COMMAND ""
  TEST_COMMAND ""
  GIT_SHALLOW 1
  UPDATE_DISCONNECTED 1
)
set(jsini_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/deps/jsini/include CACHE INTERNAL "")
set(jsini_LIBRARIES ${CMAKE_SOURCE_DIR}/deps/jsini/libjsini.a CACHE INTERNAL "")

add_library(nexer_core STATIC
  core/src/async_work.cc
  core/src/event_loop.cc
  core/src/handle.cc
  core/src/logger.cc
  core/src/tcp_client.cc
  core/src/tcp_connector.cc
  core/src/tcp_forwarder.cc
  core/src/tcp_server.cc
  core/src/timer.cc
  core/src/udp_server.cc
  core/src/url.cc
)
add_dependencies(nexer_core libuv)
target_include_directories(nexer_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
    $<BUILD_INTERFACE:${libuv_INCLUDE_DIRS}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(nexer_core PUBLIC ${libuv_LIBRARIES})

add_library(nexer_http STATIC
  http/src/http_client.cc
  http/src/http_message.cc
  http/src/http_server.cc
)
add_dependencies(nexer_http llhttp)
target_include_directories(nexer_http
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/http/include>
    $<BUILD_INTERFACE:${llhttp_INCLUDE_DIRS}>
    $<BUILD_INTERFACE:${CURL_INCLUDE_DIRS}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(nexer_http
  PUBLIC
    nexer_core
    ${llhttp_LIBRARIES}
    CURL::libcurl
)

add_library(nexer_runtime STATIC
  runtime/src/admin_server.cc
  runtime/src/config.cc
  runtime/src/instance_lock.cc
  runtime/src/nexer.cc
  runtime/src/orphan_reaper.cc
  runtime/src/output_file.cc
  runtime/src/process.cc
  runtime/src/process_manager.cc
  runtime/src/proxy_protocol.cc
  runtime/src/tcp_proxy.cc
)
add_dependencies(nexer_runtime jsini)
target_include_directories(nexer_runtime
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/runtime/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${jsini_INCLUDE_DIRS}
)
target_link_libraries(nexer_runtime
  PUBLIC
    nexer_core
    nexer_http
  PRIVATE
    ${jsini_LIBRARIES}
)

add_executable(nexer apps/nexer/main.cc)
add_dependencies(nexer jsini)
target_include_directories(nexer PRIVATE ${jsini_INCLUDE_DIRS})
target_link_libraries(nexer PRIVATE nexer_runtime ${jsini_LIBRARIES})
nexer_enable_debug_sign(nexer)

add_executable(nexer_http_example examples/http-server/main.cc)
target_link_libraries(nexer_http_example PRIVATE nexer_http)
nexer_enable_debug_sign(nexer_http_example)

enable_testing()

add_executable(run_tests
  tests/main.cc
  tests/support/helper.cc
  tests/support/runner.cc
  tests/test_admin_server.cc
  tests/test_async_work.cc
  tests/test_auto_start.cc
  tests/test_config.cc
  tests/test_dummy_servers.cc
  tests/test_function_list.cc
  tests/test_http_server.cc
  tests/test_memory_pool.cc
  tests/test_process.cc
  tests/test_process_manager.cc
  tests/test_proxy_protocol.cc
  tests/test_runtime.cc
  tests/test_startup_rollback.cc
  tests/test_string_buffer.cc
  tests/test_tcp_client.cc
  tests/test_tcp_connector.cc
  tests/test_tcp_proxy.cc
  tests/test_tcp_server.cc
  tests/test_timer.cc
  tests/test_udp_server.cc
  tests/test_url.cc
)
add_dependencies(run_tests jsini nexer)
target_include_directories(run_tests
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support
    ${jsini_INCLUDE_DIRS}
)
target_link_libraries(run_tests PRIVATE nexer_runtime SQLite3::SQLite3)
nexer_enable_debug_sign(run_tests)

add_test(NAME nexer_tests COMMAND run_tests)

install(DIRECTORY core/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY http/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY runtime/include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(TARGETS nexer_core nexer_http nexer_runtime nexer
  EXPORT nexer-targets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(EXPORT nexer-targets
  NAMESPACE nexer::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/nexer
)
