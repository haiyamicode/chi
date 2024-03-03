SHELL=/bin/bash 
BASE=..
BUILD_DIR=$(BASE)/build
TESTS = 00_fib.test \
    01_fib_struct.test \
    02_generics.test \
    03_destructors.test \
    04_builtins.test \
    # 05_enum_union.test \
    # 06_for_loop.test \
    # 07_trait_embed.test \

CHI = $(BUILD_DIR)/src/bin/chi
TMP = /tmp/chitests
OUTPUT_DIR = $(BUILD_DIR)/tests

%.test: %.expect %.xc
	echo Test: $*...
	$(CHI) -o $(TMP).$* $*.xc && $(TMP).$* 2>&1 >$*.output;
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