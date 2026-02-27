BUILD_DIR=build
BASE=$(shell pwd)
LOCAL_DIR=local
CHI = $(BUILD_DIR)/src/bin/chi
INPUT_FILE ?= $(LOCAL_DIR)/test.xs
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

compile_example_debug: build install
	$(CHI) -d -c $(INPUT_FILE) -o local/test -w local/build

run_example_debug: compile_example_debug
	./local/test

ast: build
	$(CHI) -a -c $(INPUT_FILE)

test: test_compiler

test_compiler: build
	(cd tests; make test)

stress: build
	(cd tests; make stress N=$(or $(N),1000))

analyzer_test: build
	(cd analyzer_tests; make test)

formatter_test: build
	(cd tests; make formatter_test)

test_all: build
	(cd tests; make test)
	(cd tests; make formatter_test)
	(cd analyzer_tests; make test)

clean:
	rm -rf $(BUILD_DIR)/* && mkdir -p $(BUILD_DIR)

compile_example: build install
	$(CHI) -c $(INPUT_FILE) -o local/test -w local/build

compile_example_safe: build install
	$(CHI) -s -c $(INPUT_FILE) -o local/test -w local/build

run_example: compile_example
	./local/test

debug_example: compile_example
	lldb -o run ./local/test

compile_example_package: build install
	$(CHI) -p $(INPUT_PACKAGE) -o local/test_package_exe -w local/build

run_example_package: compile_example_package
	./local/test_package_exe

analyze_example: build install
	$(CHI) -analyzer -c $(INPUT_FILE)

format_example: build install
	$(CHI) -f -c $(INPUT_FILE)

format_all: build install
	@echo "Formatting all .xs and .x files in src/stdlib and tests..."
	@find src/stdlib tests -type f \( -name "*.xs" -o -name "*.x" \) -print0 | while IFS= read -r -d '' file; do \
		if [ "$$file" = "tests/formatter_collapse.xs" ]; then \
			echo "Skipping $$file (formatter test input)..."; \
			continue; \
		fi; \
		echo "Formatting $$file..."; \
		$(CHI) -f -c "$$file" > "$$file.tmp" && mv "$$file.tmp" "$$file"; \
	done
	@echo "Formatting complete!"
