MALFORMED_FILES := $(wildcard *.xc)
CHI_COMPILER := ../build/src/bin/chi

.PHONY: all clean test

all: test

test:
	@echo "Running Chi analyzer tests..."
	@failed=0; \
	timeouts=0; \
	passed=0; \
	for file in $(MALFORMED_FILES); do \
		printf "Testing %-30s ... " "$$file"; \
		output=$$(timeout 5s env TIMES=1 $(CHI_COMPILER) --analyzer -c "$$file" 2>&1); \
		exit_code=$$?; \
		if [ $$exit_code -eq 124 ]; then \
			echo "FAILED (TIMEOUT - infinite loop)"; \
			timeouts=$$((timeouts + 1)); \
			failed=$$((failed + 1)); \
		elif [ $$exit_code -ne 0 ] && echo "$$output" | grep -q "error:"; then \
			echo "PASSED (errors detected correctly)"; \
			passed=$$((passed + 1)); \
		elif [ $$exit_code -ne 0 ]; then \
			echo "FAILED (crash without proper error message)"; \
			echo "  Output: $$output" | head -2; \
			failed=$$((failed + 1)); \
		elif echo "$$output" | grep -q "error:"; then \
			echo "PASSED (errors detected correctly)"; \
			passed=$$((passed + 1)); \
		else \
			echo "FAILED (no errors detected in malformed code)"; \
			failed=$$((failed + 1)); \
		fi; \
	done; \
	echo ""; \
	echo "Results: $$passed PASSED, $$failed FAILED ($$timeouts timeouts)"

clean:
	@echo "No build artifacts to clean in analyzer tests."