cmake_minimum_required(VERSION 3.15)
project(blob_arena LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# 统一输出目录，便于 web/ 静态资源定位
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# ---------------------------------------------------------------------------
# 游戏核心逻辑库（纯逻辑，无网络依赖，可被单测和压测工具复用）
# ---------------------------------------------------------------------------
file(GLOB CORE_SOURCES ${CMAKE_SOURCE_DIR}/src/core/*.cpp)
add_library(blob_core STATIC ${CORE_SOURCES})
target_include_directories(blob_core PUBLIC ${CMAKE_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# 存储层
# ---------------------------------------------------------------------------
add_library(blob_storage STATIC
  ${CMAKE_SOURCE_DIR}/src/storage/file_data_store.cpp
)
target_include_directories(blob_storage PUBLIC ${CMAKE_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# 网络层
# ---------------------------------------------------------------------------
if(WIN32)
  set(NET_SOURCES ${CMAKE_SOURCE_DIR}/src/net/poller_select.cpp)
else()
  set(NET_SOURCES ${CMAKE_SOURCE_DIR}/src/net/poller_epoll.cpp)
endif()

file(GLOB NET_COMMON ${CMAKE_SOURCE_DIR}/src/net/*.cpp)
list(REMOVE_ITEM NET_COMMON
     ${CMAKE_SOURCE_DIR}/src/net/poller_select.cpp
     ${CMAKE_SOURCE_DIR}/src/net/poller_epoll.cpp)
list(APPEND NET_SOURCES ${NET_COMMON})

add_library(blob_net STATIC ${NET_SOURCES})
target_link_libraries(blob_net PUBLIC blob_core)
target_include_directories(blob_net PUBLIC ${CMAKE_SOURCE_DIR}/src)

if(WIN32)
  target_link_libraries(blob_net PUBLIC ws2_32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(blob_net PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------------------
# 服务端可执行文件
# ---------------------------------------------------------------------------
file(GLOB SERVER_SOURCES ${CMAKE_SOURCE_DIR}/src/server/*.cpp)
add_executable(blob_server ${SERVER_SOURCES})
target_link_libraries(blob_server PRIVATE blob_net blob_storage)
target_include_directories(blob_server PRIVATE ${CMAKE_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# 单元测试（仅依赖核心逻辑）
# ---------------------------------------------------------------------------
add_executable(test_core
  ${CMAKE_SOURCE_DIR}/tests/test_core.cpp
  ${CMAKE_SOURCE_DIR}/src/server/room.cpp
  ${CMAKE_SOURCE_DIR}/src/server/lobby.cpp
)
target_link_libraries(test_core PRIVATE blob_core blob_storage)

enable_testing()
add_test(NAME core_tests COMMAND test_core)
