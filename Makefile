BUILD_DIR=build
BASE=$(shell pwd)
LOCAL_DIR=local
CHI = $(BUILD_DIR)/src/bin/chi
INPUT_FILE ?= $(LOCAL_DIR)/test.xc
BUILD_MODE ?= Debug
export CHI_ROOT=$(BASE)

all: debug

.PHONY: build

dep:
	cd build && conan install $(BASE) --build=missing

build:
	cd $(BUILD_DIR) && cmake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=${BUILD_MODE} .. && $(MAKE)

install:
	cd $(BUILD_DIR) && $(MAKE) install

asm: build
	$(CHI) -s $(INPUT_FILE) 

compile_example: build install
	$(CHI) $(INPUT_FILE) -o local/test -w local/build

run_example: compile_example
	./local/test

compile_example_debug: build install
	$(CHI) -d $(INPUT_FILE) -o local/test -w local/build

run_example_debug: compile_example_debug
	./local/test

ast: build
	$(CHI) -a $(INPUT_FILE)

test: test_compiler

test_compiler: build
	(cd tests; make test)

clean:
	rm -rf $(BUILD_DIR)/* && mkdir -p $(BUILD_DIR)
