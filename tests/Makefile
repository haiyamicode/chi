SHELL=/bin/bash
BASE=..
ROOT_DIR=$(abspath $(BASE))
BUILD_DIR=$(BASE)/build
export CHI_ROOT=$(ROOT_DIR)
TESTS_LIST = 00_fib.test \
    01_fib_struct.test \
    02_generics.test \
    03_destructors.test \
    04_builtins.test \
		05_interface.test \
    06_enum.test \
    07_loops.test \
		08_module.test \
		09_control.test \
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
		38_never_type.test \
		39_destructuring.test \
		40_fixed_array.test \
		41_format_directives.test \
		42_construct_expr.test \
		43_expr_blocks.test \
		44_span.test \
		45_typedef.test \
		46_generic_embed.test \
		47_structural_typing.test \
		48_stdlib.test \
		49_tuple.test \
		50_async_await_managed.test \
		51_deep_generics.test \
		52_async_lifecycle_stress.test \
		53_panic.test \
		54_managed_array_escape.test \
		54_managed_escape_stress.test \
		55_managed_map_escape.test \
		56_managed_lowlevel_wrapper_array_escape.test \
		57_managed_lowlevel_custom_ctor_array_escape.test \
		58_managed_lowlevel_custom_ctor_map_escape.test \
		59_contextual_variant_shorthand.test \
		60_conditional_compilation.test \
		61_package_import.test \
		62_map_init.test
TESTS ?= $(TESTS_LIST)
CHIC = $(abspath $(BUILD_DIR)/bin/chic)
CHI = $(abspath $(BUILD_DIR)/bin/chi)
TMP = /tmp/chitests
OUTPUT_DIR = $(BUILD_DIR)/tests

%.test: %.expect
	echo Test: $*...
	@if [ -f $*.xs ]; then \
		$(CHIC) -o $(TMP).$* -c $*.xs && $(TMP).$* 2>&1 >$*.output; \
	elif [ -f $*.x ]; then \
		$(CHIC) -o $(TMP).$* -c $*.x && $(TMP).$* 2>&1 >$*.output; \
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

# Special rule for enum struct test — also tests rejection cases
13_enum_struct.test: 13_enum_struct.expect
	echo Test: 13_enum_struct...
	@$(CHIC) -o $(TMP).13_enum_struct -c 13_enum_struct.xs && $(TMP).13_enum_struct 2>&1 >13_enum_struct.output
	@if [ "x`diff -qbu 13_enum_struct.expect 13_enum_struct.output`" != "x" ]; \
	then \
		echo "error in test 13_enum_struct (output mismatch)"; \
		diff -u 13_enum_struct.expect 13_enum_struct.output; \
		rm -f 13_enum_struct.output; \
		exit 1; \
	fi; \
	rm -f 13_enum_struct.output
	@failed=0; \
	for f in testdata/13_enum_struct_errors/*.xs; do \
		name=$$(basename $$f); \
		errmsg=$$($(CHIC) -o /dev/null -c $$f -w /tmp/chitest_enum 2>&1); \
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

# Special rule for construct expr test — also tests rejection cases
42_construct_expr.test: 42_construct_expr.expect
	echo Test: 42_construct_expr...
	@$(CHIC) -o $(TMP).42_construct_expr -c 42_construct_expr.xs && $(TMP).42_construct_expr 2>&1 >42_construct_expr.output
	@if [ "x`diff -qbu 42_construct_expr.expect 42_construct_expr.output`" != "x" ]; \
	then \
		echo "error in test 42_construct_expr (output mismatch)"; \
		diff -u 42_construct_expr.expect 42_construct_expr.output; \
		rm -f 42_construct_expr.output; \
		exit 1; \
	fi; \
	rm -f 42_construct_expr.output
	@failed=0; \
	for f in testdata/42_construct_expr_errors/*.xs; do \
		name=$$(basename $$f); \
		errmsg=$$($(CHIC) -o /dev/null -c $$f -w /tmp/chitest_construct 2>&1); \
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

# Special rule for trait bounds test — also tests rejection cases
21_trait_bounds.test: 21_trait_bounds.expect
	echo Test: 21_trait_bounds...
	@$(CHIC) -o $(TMP).21_trait_bounds -c 21_trait_bounds.xs && $(TMP).21_trait_bounds 2>&1 >21_trait_bounds.output
	@if [ "x`diff -qbu 21_trait_bounds.expect 21_trait_bounds.output`" != "x" ]; \
	then \
		echo "error in test 21_trait_bounds (output mismatch)"; \
		diff -u 21_trait_bounds.expect 21_trait_bounds.output; \
		rm -f 21_trait_bounds.output; \
		exit 1; \
	fi; \
	rm -f 21_trait_bounds.output
	@failed=0; \
	for f in testdata/21_trait_bound_errors/*.xs; do \
		name=$$(basename $$f); \
		errmsg=$$($(CHIC) -o /dev/null -c $$f -w /tmp/chitest_trait 2>&1); \
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
	@$(CHIC) -o $(TMP).30_safe_lifetime -c 30_safe_lifetime.xs && $(TMP).30_safe_lifetime 2>&1 >30_safe_lifetime.output
	@if [ "x`diff -qbu 30_safe_lifetime.expect 30_safe_lifetime.output`" != "x" ]; \
	then \
		echo "error in test 30_safe_lifetime (output mismatch)"; \
		diff -u 30_safe_lifetime.expect 30_safe_lifetime.output; \
		rm -f 30_safe_lifetime.output; \
		exit 1; \
	fi; \
	rm -f 30_safe_lifetime.output
	@failed=0; \
	for f in testdata/30_safe_errors/*.xs testdata/30_safe_errors/*.x; do \
		name=$$(basename $$f); \
		errmsg=$$($(CHIC) -o /dev/null -c $$f -w /tmp/chitest_safe 2>&1); \
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

# Special rule for stdlib mega-test — each file in testdata/48_stdlib/*.xs is compiled
# and run independently; output is compared to the corresponding .expect file.
48_stdlib.test:
	echo Test: 48_stdlib...
	@failed=0; \
	for f in testdata/48_stdlib/*.xs; do \
		name=$$(basename $$f .xs); \
		expect=testdata/48_stdlib/$$name.expect; \
		if [ ! -f "$$expect" ]; then \
			echo "error: missing expect file for $$name"; \
			failed=1; continue; \
		fi; \
		$(CHIC) -o $(TMP).48_stdlib_$$name -c $$f -w /tmp/chitest_stdlib_$$name 2>&1; \
		if [ $$? -ne 0 ]; then \
			echo "error: $$name failed to compile"; \
			failed=1; continue; \
		fi; \
		$(TMP).48_stdlib_$$name > $(TMP).48_stdlib_$$name.out 2>&1; \
		sed "s|$(ROOT_DIR)|<PROJECT_ROOT>|g" $(TMP).48_stdlib_$$name.out | \
			sed -E 's#(<PROJECT_ROOT>/[^:]+\.xs):[0-9]+#\1:0#g' | \
			sed -E '/^at /d; /^<PROJECT_ROOT>\/.* in function /d' > $(TMP).48_stdlib_$$name.norm; \
		if ! diff -qbu $$expect $(TMP).48_stdlib_$$name.norm > /dev/null; then \
			echo "error: $$name output mismatch"; \
			diff -u $$expect $(TMP).48_stdlib_$$name.norm; \
			failed=1; \
		fi; \
		rm -f $(TMP).48_stdlib_$$name.out $(TMP).48_stdlib_$$name.norm; \
	done; \
	if [ "$$failed" = "1" ]; then exit 1; fi

# Panic tests: each file in testdata/53_panic/*.xs must compile, panic at runtime,
# and include the expected panic text from `// expect-panic: ...`.
53_panic.test:
	echo Test: 53_panic...
	@failed=0; \
	for f in testdata/53_panic/*.xs; do \
		name=$$(basename $$f .xs); \
		expected=$$(grep '// expect-panic:' $$f | head -1 | sed 's/.*expect-panic: *//'); \
		if [ -z "$$expected" ]; then \
			echo "error: missing expect-panic comment in $$name"; \
			failed=1; continue; \
		fi; \
		$(CHIC) -o $(TMP).53_panic_$$name -c $$f -w /tmp/chitest_panic_$$name >/tmp/chitest_panic_compile_$$name.out 2>&1; \
		if [ $$? -ne 0 ]; then \
			echo "error: $$name failed to compile"; \
			cat /tmp/chitest_panic_compile_$$name.out; \
			failed=1; continue; \
		fi; \
		$(TMP).53_panic_$$name > $(TMP).53_panic_$$name.out 2>&1; \
		rc=$$?; \
		if [ $$rc -eq 0 ]; then \
			echo "error: $$name should have panicked but exited successfully"; \
			cat $(TMP).53_panic_$$name.out; \
			failed=1; \
		elif ! grep -q "$$expected" $(TMP).53_panic_$$name.out; then \
			echo "error: $$name wrong panic output (expected '$$expected')"; \
			cat $(TMP).53_panic_$$name.out; \
			failed=1; \
		else \
			grep '// expect-output:' $$f | sed 's/.*expect-output: *//' > $(TMP).53_panic_$$name.patterns; \
			while IFS= read -r line; do \
				if ! grep -q "$$line" $(TMP).53_panic_$$name.out; then \
					echo "error: $$name missing expected output '$$line'"; \
					cat $(TMP).53_panic_$$name.out; \
					failed=1; \
				fi; \
			done < $(TMP).53_panic_$$name.patterns; \
			rm -f $(TMP).53_panic_$$name.patterns; \
		fi; \
		rm -f $(TMP).53_panic_$$name.out /tmp/chitest_panic_compile_$$name.out; \
	done; \
	if [ "$$failed" = "1" ]; then exit 1; fi

# Expr block tests — also verify invalid if-expression forms are rejected.
43_expr_blocks.test: 43_expr_blocks.expect
	echo Test: 43_expr_blocks...
	@$(CHIC) -o $(TMP).43_expr_blocks -c 43_expr_blocks.xs && $(TMP).43_expr_blocks 2>&1 >43_expr_blocks.output
	@if [ "x`diff -qbu 43_expr_blocks.expect 43_expr_blocks.output`" != "x" ]; \
	then \
		echo "error in test 43_expr_blocks (output mismatch)"; \
		diff -u 43_expr_blocks.expect 43_expr_blocks.output; \
		rm -f 43_expr_blocks.output; \
		exit 1; \
	fi; \
	rm -f 43_expr_blocks.output
	@failed=0; \
	for f in testdata/43_expr_blocks_errors/*.xs; do \
		name=$$(basename $$f); \
		errmsg=$$($(CHIC) -o /dev/null -c $$f -w /tmp/chitest_expr_blocks 2>&1); \
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
	@$(CHIC) -o $(TMP).18_gc_escape -c 18_gc_escape.x && $(TMP).18_gc_escape 2>&1 >18_gc_escape.output
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
	$(CHIC) -p testdata/15_package -o $(TMP).15_package && $(TMP).15_package 2>&1 >15_package.output;
	@if [ "x`diff -qbu 15_package.expect 15_package.output`" != "x" ]; \
	then \
		echo "error in test 15_package"; \
		diff -u 15_package.expect 15_package.output; \
		exit 1; \
	fi; \
	rm -f 15_package.output testdata/15_package/15_package; \
	export PATH="$(dir $(CHI)):$$PATH"; \
	T=/tmp/chitest_pkg; \
	chi_build_check() { \
		local label="$$1" dir="$$2" bin="$$3"; shift 3; \
		(cd "$$dir" && "$$@") > $$T.out 2>&1 || { echo "error in test 15_package ($$label)"; cat $$T.out; exit 1; }; \
		"$$bin" > $$T.output 2>&1; \
		diff -qbu 15_package.expect $$T.output >/dev/null || { echo "error in test 15_package ($$label output mismatch)"; diff -u 15_package.expect $$T.output; exit 1; }; \
		[ ! -e testdata/15_package/15_package ] || { echo "error in test 15_package ($$label spilled output)"; rm -f testdata/15_package/15_package; exit 1; }; \
		rm -f $$T.out $$T.output; \
	}; \
	chi_run_check() { \
		local label="$$1" dir="$$2"; shift 2; \
		(cd "$$dir" && "$$@") > $$T.out 2>&1 || { echo "error in test 15_package ($$label)"; cat $$T.out; exit 1; }; \
		diff -qbu 15_package.expect $$T.out >/dev/null || { echo "error in test 15_package ($$label output mismatch)"; diff -u 15_package.expect $$T.out; exit 1; }; \
		rm -f $$T.out; \
	}; \
	chi_build_check "chi build" testdata/15_package $$T.bin \
		$(CHI) build --output $$T.bin --cwd $$T.build; \
	chi_build_check "chi build explicit path" . $$T.path_bin \
		$(CHI) build testdata/15_package --output $$T.path_bin --cwd $$T.path_build; \
	chi_build_check "chi build release" testdata/15_package $$T.rel_bin \
		$(CHI) build --release --strip --output $$T.rel_bin --cwd $$T.rel_build; \
	chi_build_check "chi build from src" testdata/15_package/src $$T.src_bin \
		$(CHI) build --output $$T.src_bin --cwd $$T.src_build; \
	chi_run_check "chi run" . \
		$(CHI) run testdata/15_package --cwd $$T.run_build; \
	chi_run_check "chi run from pkg dir" testdata/15_package \
		$(CHI) run --cwd $$T.run_cwd_build; \
	chi_build_check "chi install" testdata/15_package $$T.install_bin/15_package \
		env CHI_PATH=$$T.install_bin CHI_SRC=$$T.install_src $(CHI) install --cwd $$T.install_build; \
	if [ ! -e $$T.install_src/package.jsonc ] || [ ! -e $$T.install_src/main.xs ]; then \
		echo "error in test 15_package (chi install source missing files)"; exit 1; \
	fi; \
	if [ ! -e $$T.install_src/assets/extra.txt ] || [ ! -e $$T.install_src/assets/nested/deep.txt ] || [ -e $$T.install_src/notes.txt ] || [ -e $$T.install_src/.gitignore ] || [ -e $$T.install_src/build/main.o ]; then \
		echo "error in test 15_package (chi install source filtering mismatch)"; exit 1; \
	fi; \
	rm -f $$T.bin $$T.path_bin $$T.rel_bin $$T.src_bin testdata/15_package/15_package; \
	rm -rf $$T.build $$T.build.dSYM $$T.path_build $$T.path_build.dSYM $$T.rel_build $$T.rel_build.dSYM $$T.src_build $$T.src_build.dSYM \
		$$T.run_build $$T.run_build.dSYM $$T.run_cwd_build $$T.run_cwd_build.dSYM \
		$$T.install_build $$T.install_build.dSYM $$T.install_bin $$T.install_src

# Package import test: uses temp CHI_HOME to test package registry auto-discovery
61_package_import.test: 61_package_import.expect
	echo Test: 61_package_import...
	@PKG_HOME=/tmp/chitest_pkg_home; \
	rm -rf $$PKG_HOME; \
	mkdir -p $$PKG_HOME/src/stdlib; \
	cp -r $(ROOT_DIR)/src/stdlib/* $$PKG_HOME/src/stdlib/; \
	mkdir -p $$PKG_HOME/src/test_registry/mylib; \
	cp -r testdata/61_package_import/test_registry/mylib/* $$PKG_HOME/src/test_registry/mylib/; \
	CHI_ROOT=$$PKG_HOME $(CHIC) -o $(TMP).61_package_import -c 61_package_import.xs && \
	$(TMP).61_package_import 2>&1 >61_package_import.output; \
	if [ "x`diff -qbu 61_package_import.expect 61_package_import.output`" != "x" ]; then \
		echo "error in test 61_package_import"; \
		diff -u 61_package_import.expect 61_package_import.output; \
		rm -rf $$PKG_HOME $(TMP).61_package_import 61_package_import.output; \
		exit 1; \
	fi; \
	if CHI_ROOT=$$PKG_HOME $(CHIC) -o $(TMP).61_pkg_direct -c testdata/61_package_import/reject_direct_import.xs >/dev/null 2>&1; then \
		echo "error: direct module import should have been rejected"; \
		rm -rf $$PKG_HOME $(TMP).61_package_import $(TMP).61_pkg_direct 61_package_import.output; \
		exit 1; \
	fi; \
	rm -rf $$PKG_HOME $(TMP).61_package_import $(TMP).61_pkg_direct 61_package_import.output

all: test

test: $(TESTS)
	@echo "tests passed"

# Formatter idempotency test: verify format(file) == file for all test sources
# Excludes 15_package (uses -p flag, no single source file)
FORMAT_TESTS = $(patsubst %.test,%,$(filter-out 15_package.test 42_construct_expr.test 48_stdlib.test 53_panic.test 61_package_import.test,$(TESTS_LIST)))
test_formatter:
	@echo "Formatter idempotency test..."
	@fail=0; \
	for t in $(FORMAT_TESTS); do \
		src=$$t.xs; [ -f $$src ] || src=$$t.x; \
		$(CHIC) -f --formatter-no-semantic -c $$src > $(TMP).$$t.fmt 2>/dev/null; \
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
	@$(CHIC) -f -c formatter_collapse.xs > $(TMP).formatter_collapse.fmt 2>/dev/null; \
	if ! diff -q formatter_collapse.expected $(TMP).formatter_collapse.fmt >/dev/null 2>&1; then \
		echo "FAIL formatter_collapse (output differs from expected)"; \
		diff -u formatter_collapse.expected $(TMP).formatter_collapse.fmt; \
		rm -f $(TMP).formatter_collapse.fmt; \
		exit 1; \
	fi; \
	rm -f $(TMP).formatter_collapse.fmt; \
	echo "formatter collapse test passed"

formatter_semantic_collapse_test:
	@echo "Formatter semantic collapse test..."
	@$(CHIC) -f -c formatter_semantic_collapse.xs > $(TMP).formatter_semantic_collapse.fmt 2>/dev/null; \
	if ! diff -q formatter_semantic_collapse.expected $(TMP).formatter_semantic_collapse.fmt >/dev/null 2>&1; then \
		echo "FAIL formatter_semantic_collapse (output differs from expected)"; \
		diff -u formatter_semantic_collapse.expected $(TMP).formatter_semantic_collapse.fmt; \
		rm -f $(TMP).formatter_semantic_collapse.fmt; \
		exit 1; \
	fi; \
	rm -f $(TMP).formatter_semantic_collapse.fmt; \
	echo "formatter semantic collapse test passed"

# Dedicated heavy lifecycle stress suite. Keep it out of TESTS_LIST.
define STRESS_TEST
	@echo "Test: stress/$(1)..."
	@tmp_base=$$(mktemp /tmp/chitests.$(1).XXXXXX); \
	bin="$${tmp_base}.bin"; \
	out="$${tmp_base}.out"; \
	$(CHIC) --debug-allocator-runtime -o "$$bin" -c stress/$(1).xs || exit $$?; \
	"$$bin" > "$$out" 2>&1; \
	status=$$?; \
	if [ $$status -ne 0 ]; then \
		echo "error in test stress/$(1) (compile/run)"; \
		cat "$$out"; \
		rm -f "$$bin" "$$out"; \
		exit $$status; \
	fi; \
	if [ ! -f "$$out" ]; then \
		echo "error in test stress/$(1) (missing output file)"; \
		rm -f "$$bin"; \
		exit 1; \
	fi; \
	checkpoints=$$(grep -c '^checkpoint ' "$$out" 2>/dev/null || true); \
	if [ $$checkpoints -ne 10 ]; then \
		echo "error in test stress/$(1) (missing checkpoints)"; \
		cat "$$out"; \
		rm -f "$$bin" "$$out"; \
		exit 1; \
	fi; \
	if ! diff -qbu stress/$(1).expect "$$out" >/dev/null; then \
		echo "error in test stress/$(1) (output mismatch)"; \
		diff -u stress/$(1).expect "$$out"; \
		rm -f "$$bin" "$$out"; \
		exit 1; \
	fi; \
	rm -f "$$bin" "$$out"
endef

stress:
	$(call STRESS_TEST,collection_memory_leak_stress)
	$(call STRESS_TEST,lambda_error_recovery_stress)
	@echo "stress allocator tracking ok"
