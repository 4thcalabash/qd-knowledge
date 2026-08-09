# SPSC 队列性能测试 —— 多实现版本统一构建
#
# 新增实现：在 include/ 下新建 spsc_<name>.hpp（定义 spsc::Queue），
# 再把 <name> 追加到 IMPLS，即可构建出 bin/spsc_<name>，main.cpp 无需改动。
#
# CXXFLAGS 可从命令行覆盖：make CXXFLAGS='-O3 -march=native ...'
# -fno-omit-frame-pointer：保留帧指针，供 perf record --call-graph fp 采样调用栈

CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -pthread -fno-omit-frame-pointer -Iinclude
BIN      := bin
IMPLS    := naive_mutex

TARGETS := $(addprefix $(BIN)/spsc_,$(IMPLS))

.PHONY: all clean

all: $(TARGETS)

$(BIN)/spsc_%: main.cpp include/spsc_%.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) -DSPSC_HEADER='"spsc_$*.hpp"' $< -o $@

$(BIN):
	mkdir -p $(BIN)

clean:
	rm -rf $(BIN)
