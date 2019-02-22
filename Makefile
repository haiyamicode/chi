BUILD_DIR=cmake-build-debug
TEST_DIR=$(BUILD_DIR)
CHI = $(BUILD_DIR)/src/chi

all: debug

.PHONY: build

build:
	cd cmake-build-debug && cmake .. && $(MAKE)

asm: build
	$(CHI) -s $(TEST_DIR)/test.cx

run: build
	$(CHI) $(TEST_DIR)/test.cx

debug: build
	$(CHI) -d $(TEST_DIR)/test.cx

test: test_jit

test_jit: build
	(cd tests; make test)