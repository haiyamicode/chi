BUILD_DIR=cmake-build-debug
TEST_DIR=$(BUILD_DIR)
CX = $(BUILD_DIR)/src/cx

all: debug

.PHONY: build

build:
	cd cmake-build-debug && cmake .. && $(MAKE)

asm: build
	$(CX) -s $(TEST_DIR)/test.cx

run: build
	$(CX) $(TEST_DIR)/test.cx

debug: build
	$(CX) -d $(TEST_DIR)/test.cx

test: build
	(cd tests; make test)