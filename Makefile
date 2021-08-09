BUILD_DIR=build
BASE=$(shell pwd)
TEST_DIR=$(BUILD_DIR)
CHI = $(BUILD_DIR)/src/bin/chi
TEST_FILE ?= $(TEST_DIR)/test.chi
BUILD_MODE ?= Debug

all: debug

.PHONY: build

dep:
	cd build && conan install $(BASE) -s compiler.libcxx=libstdc++11

build:
	cd $(BUILD_DIR) && cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=${BUILD_MODE} .. && $(MAKE)

asm: build
	$(CHI) -s $(TEST_FILE) 

run: build
	$(CHI) $(TEST_FILE)

debug: build
	$(CHI) -d $(TEST_FILE)

test: test_jit

test_jit: build
	(cd tests; make test)
