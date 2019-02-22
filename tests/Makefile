TESTS = 00_fib.test \
    01_fib_struct.test \
    02_basic_array.test \
    03_destructors.test \
    04_enum_union.test \
    05_ptr_casting.test \
    06_for_loop.test \
    07_trait_embed.test \
    08_generics.test \
    09_string_any.test \
    10_optional.test \
    11_box.test

CHI = ../cmake-build-debug/src/chi
TMP = /tmp/chitests

%.test: %.expect %.chi
	@echo Test: $*...
	@if [ "${CHI_AOT}" == "" ]; \
	then \
		$(CHI) $*.chi 2>&1 >$*.output; \
	else \
        $(CHI) -o $(TMP).$* $*.chi && $(TMP).$* 2>&1 >$*.output; \
	fi
	@if [ "x`diff -qbu $*.expect $*.output`" != "x" ]; \
	then \
        	echo "error in test $*"; \
        	diff -u $*.expect $*.output; \
		exit 1; \
	fi; \
       	rm -f $*.output

all: test

test: $(TESTS)
	@echo "tests passed"