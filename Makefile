BUILD_DIR=build
BASE=$(shell pwd)
LOCAL_DIR=local
CHI = $(BUILD_DIR)/src/bin/chi
TEST_FILE ?= $(LOCAL_DIR)/test.xc
BUILD_MODE ?= Debug

all: debug

.PHONY: build

dep:
	cd build && conan install $(BASE) --build=missing

build:
	cd $(BUILD_DIR) && cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=${BUILD_MODE} .. && $(MAKE)

install:
	cd $(BUILD_DIR) && $(MAKE) install

asm: build
	$(CHI) -s $(TEST_FILE) 

run: build
	$(CHI) $(TEST_FILE) -w local/build

debug: build
	$(CHI) -d $(TEST_FILE) -w local/build

ast: build
	$(CHI) -a $(TEST_FILE)

test: test_jit

test_jit: build
	(cd tests; make test)

clean:
	rm -rf $(BUILD_DIR)/* && mkdir -p $(BUILD_DIR)
