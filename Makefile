BUILD_DIR=build
BASE=$(shell pwd)
LOCAL_DIR=local
CHI = $(BUILD_DIR)/src/bin/chi
INPUT_FILE ?= $(LOCAL_DIR)/test.xc
INPUT_PACKAGE ?= $(LOCAL_DIR)/test_package
BUILD_MODE ?= Debug
export CHI_ROOT=$(BASE)

all: debug

.PHONY: build

dep:
	cd build && conan install $(BASE) --build=missing

build:
	cd $(BUILD_DIR) && cmake -G "Unix Makefiles" -DCMAKE_EXPORT_COMPILE_COMMANDS=1 -DCMAKE_BUILD_TYPE=${BUILD_MODE} .. && $(MAKE)

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

compile_example: build install
	$(CHI) $(INPUT_FILE) -o local/test -w local/build

run_example: compile_example
	./local/test

compile_example_package: build install
	$(CHI) -p $(INPUT_PACKAGE) -o local/test_package_exe -w local/build

run_example_package: compile_example_package
	./local/test_package_exe
