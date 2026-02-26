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
		23_operator_overloading.test \
		24_async_await.test \
		25_default_args.test \
		26_lambda_inference.test \
		28_global_let.test \
		29_error_handling.test \
		30_safe_lifetime.test \
		31_pointer_arithmetic.test \
		32_type_narrowing.test \
		33_iterators.test \
		34_rune.test \
		35_slice.test \
		36_optional_ops.test \
		37_pack_expansion.test \
		38_never_type.test
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

# Special rule for trait bounds test — also tests rejection cases
21_trait_bounds.test: 21_trait_bounds.expect
	echo Test: 21_trait_bounds...
	@$(CHI) -o $(TMP).21_trait_bounds -c 21_trait_bounds.xc && $(TMP).21_trait_bounds 2>&1 >21_trait_bounds.output
	@if [ "x`diff -qbu 21_trait_bounds.expect 21_trait_bounds.output`" != "x" ]; \
	then \
		echo "error in test 21_trait_bounds (output mismatch)"; \
		diff -u 21_trait_bounds.expect 21_trait_bounds.output; \
		rm -f 21_trait_bounds.output; \
		exit 1; \
	fi; \
	rm -f 21_trait_bounds.output
	@failed=0; \
	for f in testdata/21_trait_bound_errors/*.xc; do \
		name=$$(basename $$f); \
		errmsg=$$($(CHI) -o /dev/null -c $$f -w /tmp/chitest_trait 2>&1); \
		if [ $$? -eq 0 ]; then \
			echo "error: $$name should have been rejected but compiled successfully"; \
			failed=1; \
		else \
			expected=$$(grep '// expect-error:' $$f | head -1 | sed 's/.*expect-error: *//'); \
			if [ -n "$$expected" ] && ! echo "$$errmsg" | grep -q "$$expected"; then \
				echo "error: $$name wrong error (expected '$$expected')"; \
				echo "  got: $$(echo "$$errmsg" | head -1)"; \
				failed=1; \
			fi; \
		fi; \
	done; \
	if [ "$$failed" = "1" ]; then exit 1; fi

# Special rule for safe lifetime test.
# Also tests error cases: each file in testdata/30_safe_errors/ must fail to compile.
30_safe_lifetime.test: 30_safe_lifetime.expect
	echo Test: 30_safe_lifetime...
	@$(CHI) -o $(TMP).30_safe_lifetime -c 30_safe_lifetime.xc && $(TMP).30_safe_lifetime 2>&1 >30_safe_lifetime.output
	@if [ "x`diff -qbu 30_safe_lifetime.expect 30_safe_lifetime.output`" != "x" ]; \
	then \
		echo "error in test 30_safe_lifetime (output mismatch)"; \
		diff -u 30_safe_lifetime.expect 30_safe_lifetime.output; \
		rm -f 30_safe_lifetime.output; \
		exit 1; \
	fi; \
	rm -f 30_safe_lifetime.output
	@failed=0; \
	for f in testdata/30_safe_errors/*.xc testdata/30_safe_errors/*.x; do \
		name=$$(basename $$f); \
		errmsg=$$($(CHI) -o /dev/null -c $$f -w /tmp/chitest_safe 2>&1); \
		if [ $$? -eq 0 ]; then \
			echo "error: $$name should have been rejected but compiled successfully"; \
			failed=1; \
		else \
			expected=$$(grep '// expect-error:' $$f | head -1 | sed 's/.*expect-error: *//'); \
			if [ -n "$$expected" ] && ! echo "$$errmsg" | grep -q "$$expected"; then \
				echo "error: $$name wrong error (expected '$$expected')"; \
				echo "  got: $$(echo "$$errmsg" | head -1)"; \
				failed=1; \
			fi; \
		fi; \
	done; \
	if [ "$$failed" = "1" ]; then exit 1; fi

# Special rule for GC tests - finalization order is non-deterministic,
# so sort destruction lines (everything after "Test completed") before comparing.
18_gc_escape.test: 18_gc_escape.expect
	echo Test: 18_gc_escape...
	@$(CHI) -o $(TMP).18_gc_escape -c 18_gc_escape.x && $(TMP).18_gc_escape 2>&1 >18_gc_escape.output
	@grep -v '^\[gc\]' 18_gc_escape.output > 18_gc_escape.out.tmp
	@grep -v '^\[gc\]' 18_gc_escape.expect > 18_gc_escape.exp.tmp
	@grep '^\[gc\]' 18_gc_escape.output | sort > 18_gc_escape.out_gc.tmp
	@grep '^\[gc\]' 18_gc_escape.expect | sort > 18_gc_escape.exp_gc.tmp
	@failed=0; \
	if ! diff -qbu 18_gc_escape.exp.tmp 18_gc_escape.out.tmp > /dev/null 2>&1; then \
		echo "error in test 18_gc_escape (logic)"; \
		diff -u 18_gc_escape.exp.tmp 18_gc_escape.out.tmp; \
		failed=1; \
	fi; \
	if ! diff -qbu 18_gc_escape.exp_gc.tmp 18_gc_escape.out_gc.tmp > /dev/null 2>&1; then \
		echo "error in test 18_gc_escape (gc)"; \
		diff -u 18_gc_escape.exp_gc.tmp 18_gc_escape.out_gc.tmp; \
		failed=1; \
	fi; \
	rm -f 18_gc_escape.output 18_gc_escape.*.tmp; \
	if [ "$$failed" = "1" ]; then exit 1; fi

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

# Formatter idempotency test: verify format(file) == file for all test sources
# Excludes 15_package (uses -p flag, no single source file)
FORMAT_TESTS = $(patsubst %.test,%,$(filter-out 15_package.test,$(TESTS_LIST)))
formatter_test:
	@echo "Formatter idempotency test..."
	@fail=0; \
	for t in $(FORMAT_TESTS); do \
		src=$$t.xc; [ -f $$src ] || src=$$t.x; \
		$(CHI) -f -c $$src > $(TMP).$$t.fmt 2>/dev/null; \
		if ! diff -q $$src $(TMP).$$t.fmt >/dev/null 2>&1; then \
			echo "FAIL $$t (formatter output differs from source)"; \
			diff -u $$src $(TMP).$$t.fmt | head -20; \
			fail=1; \
		fi; \
		rm -f $(TMP).$$t.fmt; \
	done; \
	if [ $$fail -eq 0 ]; then echo "formatter tests passed"; else exit 1; fi

# Formatter collapse test: verify formatting collapses redundant syntax
formatter_collapse_test:
	@echo "Formatter collapse test..."
	@$(CHI) -f -c formatter_collapse.xc > $(TMP).formatter_collapse.fmt 2>/dev/null; \
	if ! diff -q formatter_collapse.expected $(TMP).formatter_collapse.fmt >/dev/null 2>&1; then \
		echo "FAIL formatter_collapse (output differs from expected)"; \
		diff -u formatter_collapse.expected $(TMP).formatter_collapse.fmt; \
		rm -f $(TMP).formatter_collapse.fmt; \
		exit 1; \
	fi; \
	rm -f $(TMP).formatter_collapse.fmt; \
	echo "formatter collapse test passed"

# Stress test: run all tests N times to catch non-deterministic bugs
# Usage: make stress N=1000
N ?= 1000
STRESS_TESTS = $(patsubst %.test,%,$(filter-out 15_package.test 18_gc_escape.test,$(TESTS_LIST)))
stress:
	@echo "Stress testing ($(N) iterations)..."
	@for i in $$(seq 1 $(N)); do \
		for t in $(STRESS_TESTS); do \
			src=$$t.xc; [ -f $$src ] || src=$$t.x; \
			$(CHI) -o $(TMP).$$t -c $$src && $(TMP).$$t >$(TMP).$$t.out 2>&1 || { echo "FAIL $$t at iteration $$i (compile/run)"; exit 1; }; \
			diff -qbu $$t.expect $(TMP).$$t.out >/dev/null || { echo "FAIL $$t at iteration $$i (output mismatch)"; diff -u $$t.expect $(TMP).$$t.out; exit 1; }; \
		done; \
		if [ $$((i % 100)) -eq 0 ]; then echo "  $$i/$(N) OK"; fi; \
	done; \
	echo "Stress test: $(N)/$(N) PASSED"