# =============================================================================
#  Blob Arena（吞噬大作战）构建脚本
#  支持 MinGW make（Windows 本地开发）与 GNU make（Linux 部署）
#
#  用法：
#    mingw32-make            # 构建全部
#    mingw32-make server     # 只构建服务端
#    mingw32-make test       # 只构建单元测试
#    mingw32-make run        # 构建并运行服务端
#    mingw32-make testrun    # 构建并运行单元测试
#    mingw32-make clean      # 清理
# =============================================================================

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Isrc
LDLIBS   ?=

ifeq ($(OS),Windows_NT)
  PLATFORM_EXCLUDE := src/net/poller_epoll.cpp
  # winmm：timeBeginPeriod(1) 提升 Windows 定时器分辨率（帧节拍精度）
  LDLIBS  += -lws2_32 -lwinmm
  EXE     := .exe
  CLEAN   := del /Q bin\*.exe 2>nul & exit /b 0
else
  PLATFORM_EXCLUDE := src/net/poller_select.cpp
  LDLIBS  += -lpthread
  EXE     :=
  CLEAN   := rm -f bin/blob_server bin/test_core
endif

ALL_NET      := $(wildcard src/net/*.cpp)
NET_SRCS     := $(filter-out $(PLATFORM_EXCLUDE),$(ALL_NET))
CORE_SRCS    := $(wildcard src/core/*.cpp)
STORAGE_SRCS := src/storage/file_data_store.cpp
SERVER_SRCS  := $(wildcard src/server/*.cpp)
ROOM_SRCS    := src/server/room.cpp src/server/lobby.cpp
BIN          := bin/blob_server$(EXE)
TESTBIN      := bin/test_core$(EXE)

.PHONY: all server test run testrun clean

all: $(BIN) $(TESTBIN)

server: $(BIN)

test: $(TESTBIN)

$(BIN): $(CORE_SRCS) $(STORAGE_SRCS) $(NET_SRCS) $(SERVER_SRCS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(TESTBIN): $(CORE_SRCS) $(STORAGE_SRCS) $(ROOM_SRCS) tests/test_core.cpp
	$(CXX) $(CXXFLAGS) $(CORE_SRCS) $(STORAGE_SRCS) $(ROOM_SRCS) tests/test_core.cpp -o $@ $(LDLIBS)

run: $(BIN)
	$(BIN)

testrun: $(TESTBIN)
	$(TESTBIN)

clean:
	-$(CLEAN)
