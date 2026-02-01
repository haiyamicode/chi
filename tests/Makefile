SHELL=/bin/bash 
BASE=..
BUILD_DIR=$(BASE)/build
TESTS_LIST = 00_fib.test \
    01_fib_struct.test \
    02_generics.test \
    03_destructors.test \
    04_builtins.test \
		05_interface.test \
    06_enum.test \
    07_loops.test \
		08_module.test \
		09_switch.test \
		10_copy.test \
		11_copy_builtins.test \
		12_references.test \
		13_enum_struct.test \
		14_static_method.test \
		15_package.test \
		16_lambda.test \
		17_managed_basics.test \
		18_gc_escape.test \
		19_type_params.test \
		20_numeric_types.test \
		21_trait_bounds.test \
		22_auto_lifecycle.test \
		23_operator_overloading.test
TESTS ?= $(TESTS_LIST)
CHI = $(BUILD_DIR)/src/bin/chi
TMP = /tmp/chitests
OUTPUT_DIR = $(BUILD_DIR)/tests

%.test: %.expect
	echo Test: $*...
	@if [ -f $*.xc ]; then \
		$(CHI) -o $(TMP).$* -c $*.xc && $(TMP).$* 2>&1 >$*.output; \
	elif [ -f $*.x ]; then \
		$(CHI) -o $(TMP).$* -c $*.x && $(TMP).$* 2>&1 >$*.output; \
	else \
		echo "No source file found for test $*"; \
		exit 1; \
	fi
	@if [ "x`diff -qbu $*.expect $*.output`" != "x" ]; \
	then \
        	echo "error in test $*"; \
        	diff -u $*.expect $*.output; \
		exit 1; \
	fi; \
       	rm -f $*.output

# Special rule for GC tests - finalization order is non-deterministic,
# so sort destruction lines (everything after "Test completed") before comparing.
18_gc_escape.test: 18_gc_escape.expect
	echo Test: 18_gc_escape...
	@$(CHI) -o $(TMP).18_gc_escape -c 18_gc_escape.x && $(TMP).18_gc_escape 2>&1 >18_gc_escape.output
	@awk '/^Test completed$$/{found=1; print; next} found{print | "sort"} !found{print}' 18_gc_escape.output > 18_gc_escape.sorted
	@mv 18_gc_escape.sorted 18_gc_escape.output
	@if [ "x`diff -qbu 18_gc_escape.expect 18_gc_escape.output`" != "x" ]; \
	then \
		echo "error in test 18_gc_escape"; \
		diff -u 18_gc_escape.expect 18_gc_escape.output; \
		exit 1; \
	fi; \
	rm -f 18_gc_escape.output

# Special rule for package tests
15_package.test: 15_package.expect
	echo Test: 15_package...
	$(CHI) -p testdata/15_package -o $(TMP).15_package && $(TMP).15_package 2>&1 >15_package.output;
	@if [ "x`diff -qbu 15_package.expect 15_package.output`" != "x" ]; \
	then \
        	echo "error in test 15_package"; \
        	diff -u 15_package.expect 15_package.output; \
		exit 1; \
	fi; \
       	rm -f 15_package.output

all: test

test: $(TESTS)
	@echo "tests passed"