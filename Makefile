CX = cmake-build-debug/src/cx

all: debug

.PHONY: build

build:
	cd cmake-build-debug && cmake .. && $(MAKE)

asm: build
	$(CX) -s programs/test.cx

run: build
	$(CX) programs/test.cx

debug: build
	$(CX) -d programs/test.cx

test: build
	(cd tests; make test)