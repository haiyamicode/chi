BUILD_DIR=build
BASE=$(shell pwd)
LOCAL_DIR=local
CHIC = $(BUILD_DIR)/src/bin/chic
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

rebuild:
	. $(LOCAL_DIR)/init_env.sh && cd $(BUILD_DIR) && rm -f CMakeCache.txt && cmake -G "Unix Makefiles" -DCMAKE_EXPORT_COMPILE_COMMANDS=1 -DCMAKE_BUILD_TYPE=${BUILD_MODE} $(CMAKE_ARGS) .. && $(MAKE)

install:
	cd $(BUILD_DIR) && $(MAKE) install

compile_example_debug: build install
	$(CHIC) -d -c $(INPUT_FILE) -o local/test -w local/build

run_example_debug: compile_example_debug
	./local/test

debug_compile_example: build install
	lldb -o run -o "bt all" -o quit -- $(CHIC) -d -c $(INPUT_FILE) -o local/test -w local/build

ast: build
	$(CHIC) -a -c $(INPUT_FILE)

test: test_compiler

test_compiler: build
	(cd tests; make test)

stress: build
	(cd tests; make stress N=$(or $(N),1000))

analyzer_test: build
	(cd analyzer_tests; make test)

formatter_test: build
	(cd tests; make formatter_test)
	(cd tests; make formatter_collapse_test)
	(cd tests; make formatter_semantic_collapse_test)

test_all: build
	(cd tests; make test)
	(cd tests; make formatter_test)
	(cd tests; make formatter_collapse_test)
	(cd tests; make formatter_semantic_collapse_test)
	(cd analyzer_tests; make test)

clean:
	rm -rf $(BUILD_DIR)/* && mkdir -p $(BUILD_DIR)

compile_example: build install
	$(CHIC) -c $(INPUT_FILE) -o local/test -w local/build

compile_example_safe: build install
	$(CHIC) -s -c $(INPUT_FILE) -o local/test -w local/build

run_example: compile_example
	./local/test

debug_example: compile_example
	lldb -o run ./local/test

compile_example_package: build install
	$(CHIC) -p $(INPUT_PACKAGE) -o local/test_package_exe -w local/build

run_example_package: compile_example_package
	./local/test_package_exe

analyze_example: build install
	$(CHIC) -analyzer -c $(INPUT_FILE)

format_example: build install
	$(CHIC) -f -c $(INPUT_FILE)

format_all: build install
	@echo "Formatting all .xs and .x files in src/stdlib and tests..."
	@find src/stdlib tests -type f \( -name "*.xs" -o -name "*.x" \) -print0 | while IFS= read -r -d '' file; do \
		case "$$file" in \
			tests/formatter_collapse.xs|\
			tests/formatter_semantic_collapse.xs|\
			tests/42_construct_expr.xs|\
			tests/testdata/*_errors/*) \
				echo "Skipping $$file (formatter test input)..."; \
				continue; \
				;; \
		esac; \
		echo "Formatting $$file..."; \
		$(CHIC) -f -c "$$file" > "$$file.tmp" || { rm -f "$$file.tmp"; exit 1; }; \
		mv "$$file.tmp" "$$file"; \
	done
	@echo "Formatting complete!"
