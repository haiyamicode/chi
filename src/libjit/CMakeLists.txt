enable_language(C)

include_directories(.)
include_directories(attic)
include_directories(dpas)
include_directories(include)
include_directories(include/jit)
include_directories(jit)

include(CMakeCheckCompilerFeatures.cmake)
option(DISABLE_JIT "Disable JIT and use the interpreter" OFF)
if (DISABLE_JIT)
    set(USE_LIBJIT_INTERPRETER 1)
endif ()
option(ENABLE_SIGNALS "Use the OS signals for exception handling" ON)
if (ENABLE_SIGNALS)
    set(JIT_USE_SIGNALS 1)
endif ()

configure_file(config.h.cmake.in config.h)
include_directories(${CMAKE_BINARY_DIR})

add_library(jit
        attic/jit-apply-alpha.c
        attic/jit-apply-alpha.h
        attic/jit-gen-alpha.h
        attic/jit-rules-alpha.c
        attic/jit-rules-alpha.h
        attic/jit-rules-alpha.ins
        dpas/dpas-builtin.c
        dpas/dpas-function.c
        dpas/dpas-internal.h
        dpas/dpas-main.c
        dpas/dpas-parser.c
        dpas/dpas-parser.h
        dpas/dpas-scanner.c
        dpas/dpas-scope.c
        dpas/dpas-scope.h
        dpas/dpas-semantics.h
        dpas/dpas-types.c
        dpas/dpas-types.h
        include/jit/jit-apply.h
        include/jit/jit-arch-arm.h
        include/jit/jit-arch-generic.h
        include/jit/jit-arch-x86-64.h
        include/jit/jit-arch-x86.h
        include/jit/jit-block.h
        include/jit/jit-common.h
        include/jit/jit-context.h
        include/jit/jit-debugger.h
        include/jit/jit-defs.h
        include/jit/jit-dump.h
        include/jit/jit-dynamic.h
        include/jit/jit-elf.h
        include/jit/jit-except.h
        include/jit/jit-function.h
        include/jit/jit-init.h
        include/jit/jit-insn.h
        include/jit/jit-intrinsic.h
        include/jit/jit-memory.h
        include/jit/jit-meta.h
        include/jit/jit-objmodel-private.h
        include/jit/jit-objmodel.h
        include/jit/jit-opcode-compat.h
        include/jit/jit-opcode.h
        include/jit/jit-plus.h
        include/jit/jit-type.h
        include/jit/jit-unwind.h
        include/jit/jit-util.h
        include/jit/jit-value.h
        include/jit/jit-vmem.h
        include/jit/jit-walk.h
        include/jit/jit.h
        jit/jit-alloc.c
        jit/jit-apply-arm.c
        jit/jit-apply-arm.h
        jit/jit-apply-func.h
        jit/jit-apply-rules.h
        jit/jit-apply-x86-64.c
        jit/jit-apply-x86-64.h
        jit/jit-apply-x86.c
        jit/jit-apply-x86.h
        jit/jit-apply.c
        jit/jit-bitset.c
        jit/jit-bitset.h
        jit/jit-block.c
        jit/jit-compile.c
        jit/jit-config.h
        jit/jit-context.c
        jit/jit-cpuid-x86.c
        jit/jit-cpuid-x86.h
        jit/jit-debugger.c
        jit/jit-dump.c
        jit/jit-elf-defs.h
        jit/jit-elf-read.c
        jit/jit-elf-write.c
        jit/jit-except.c
        jit/jit-function.c
        jit/jit-gen-arm.c
        jit/jit-gen-arm.h
        jit/jit-gen-x86-64.h
        jit/jit-gen-x86.h
        jit/jit-init.c
        jit/jit-insn.c
        jit/jit-internal.h
        jit/jit-interp-labels.h
        jit/jit-interp-opcode.c
        jit/jit-interp-opcode.h
        jit/jit-interp.c
        jit/jit-interp.h
        jit/jit-intrinsic.c
        jit/jit-live.c
        jit/jit-memory-cache.c
        jit/jit-memory.c
        jit/jit-meta.c
        jit/jit-objmodel.c
        jit/jit-opcode-apply.c
        jit/jit-opcode.c
        jit/jit-pool.c
        jit/jit-reg-alloc.c
        jit/jit-reg-alloc.h
        jit/jit-reg-class.c
        jit/jit-reg-class.h
        jit/jit-rules-arm.c
        jit/jit-rules-arm.h
        jit/jit-rules-interp.c
        jit/jit-rules-interp.h
        jit/jit-rules-x86-64.c
        jit/jit-rules-x86-64.h
        jit/jit-rules-x86-64.ins
        jit/jit-rules-x86.c
        jit/jit-rules-x86.h
        jit/jit-rules-x86.ins
        jit/jit-rules.c
        jit/jit-rules.h
        jit/jit-setjmp.h
        jit/jit-signal.c
        jit/jit-symbol.c
        jit/jit-thread.c
        jit/jit-thread.h
        jit/jit-type.c
        jit/jit-unwind.c
        jit/jit-util.c
        jit/jit-value.c
        jit/jit-varint.c
        jit/jit-varint.h
        jit/jit-vmem.c
        jit/jit-walk.c
        jitdynamic/jit-cpp-mangle.c
        jitdynamic/jit-dynlib.c
        jitplus/jit-plus-context.cpp
        jitplus/jit-plus-function.cpp
        jitplus/jit-plus-jump-table.cpp
        jitplus/jit-plus-value.cpp
        Makefile.am)

install(TARGETS jit DESTINATION ${CMAKE_INSTALL_PREFIX}/lib)