BUILD_DIR=cmake-build-debug
TEST_DIR=$(BUILD_DIR)
CHI = $(BUILD_DIR)/src/chi
TEST_FILE=$(TEST_DIR)/test.chi

all: debug

.PHONY: build

build:
	cd cmake-build-debug && cmake .. && $(MAKE)

asm: build
	$(CHI) -s $(TEST_FILE) 

run: build
	$(CHI) $(TEST_FILE)

debug: build
	$(CHI) -d $(TEST_FILE)

test: test_jit

test_jit: build
	(cd tests; make test)
