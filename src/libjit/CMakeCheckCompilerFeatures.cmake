INCLUDE(${CMAKE_ROOT}/Modules/CheckTypeSize.cmake)
INCLUDE(${CMAKE_ROOT}/Modules/CheckFunctionExists.cmake)

SET(AUX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/build-aux/cmake)

MESSAGE("+++++++++++++++++++++++++++++++++++++++++++++++++++++++++")
MESSAGE("Checking compiler features:")
MESSAGE("+++++++++++++++++++++++++++++++++++++++++++++++++++++++++")

#######################################################################################

MESSAGE("+++++  Checking sizes of types")
CHECK_TYPE_SIZE("void *" SIZEOF_VOID_P)
CHECK_TYPE_SIZE(char SIZEOF_CHAR)
CHECK_TYPE_SIZE(short SIZEOF_SHORT)
CHECK_TYPE_SIZE(int SIZEOF_INT)
CHECK_TYPE_SIZE(long SIZEOF_LONG)
CHECK_TYPE_SIZE(float SIZEOF_FLOAT)
CHECK_TYPE_SIZE(double SIZEOF_DOUBLE)
CHECK_TYPE_SIZE("long double" SIZEOF_LONG_DOUBLE)
CHECK_TYPE_SIZE(size_t SIZEOF_SIZE_T)
CHECK_TYPE_SIZE(__int64 SIZEOF___INT64)
CHECK_TYPE_SIZE("long long" SIZEOF_LONG_LONG)

#######################################################################################

MESSAGE("+++++  Checking for mmap support") # check memory mmap functions
# check memory mmap functions
CHECK_FUNCTION_EXISTS(mmap HAVE_MMAP)
CHECK_FUNCTION_EXISTS(munmap HAVE_MUNMAP)
CHECK_FUNCTION_EXISTS(mprotect HAVE_MPROTECT)
IF (HAVE_MMAP AND HAVE_MUNMAP AND HAVE_PROTECT)
    SET(HAVE_ALLOC_MMAP 1 CACHE BOOL "JIT mmap ok")
ENDIF ()

#######################################################################################

MESSAGE("+++++  Checking for vsnprintf function")
CHECK_FUNCTION_EXISTS(vsnprintf HAVE_VSNPRINTF)
CHECK_FUNCTION_EXISTS(_vsnprintf HAVE__VSNPRINTF)
CHECK_FUNCTION_EXISTS(vsprintf HAVE_VSPRINTF)

#######################################################################################

MESSAGE("+++++  Checking for the POSIX unistd.h header")    # check unistd.h
CHECK_INCLUDE_FILE(unistd.h HAVE_UNISTD_H)

#######################################################################################

# check for time headers
MESSAGE("+++++  Checking for headers with time information")
CHECK_INCLUDE_FILE(sys/time.h HAVE_SYS_TIME_H)
CHECK_INCLUDE_FILE(time.h HAVE_TIME_H)
CHECK_INCLUDE_FILE(sys/resource.h HAVE_SYS_RESOURCE_H)
CHECK_FUNCTION_EXISTS(gettimeofday HAVE_GETTIMEOFDAY)

#######################################################################################
# Win32 specific
#######################################################################################

IF (WIN32)

    MESSAGE("+++++  Checking for the Win32 windows.h header")    # check windows.hfor Windows API
    CHECK_INCLUDE_FILE(windows.h HAVE_WINDOWSH)
    MESSAGE("+++++  Checking for the Win32 dbghelp.h header")    # check dbghelp.h for call stack
    CHECK_INCLUDE_FILE(dbghelp.h HAVE_DBGHELPH)
    MESSAGE("+++++  Checking for the Win32 psapi.h header")      # check psapi.h for memory info
    CHECK_INCLUDE_FILE(psapi.h HAVE_PSAPIH)

ENDIF ()

#######################################################################################
# UNIX specific
#######################################################################################

IF (UNIX)

    MESSAGE("+++++  Checking for the dlfcn.h header")  # for dlopen
    CHECK_INCLUDE_FILE(dlfcn.h HAVE_DLOPEN)
    CHECK_INCLUDE_FILE(dlfcn.h HAVE_DLFCN)

ENDIF ()

#######################################################################################
# Check computed goto
#######################################################################################

#MESSAGE("+++++  Checking computed goto")
#TRY_COMPILE(HAVE_COMPUTED_GOTO ${AUX_DIR} ${AUX_DIR}/cgoto.c)
#TRY_COMPILE(HAVE_PIC_COMPUTED_GOTO ${AUX_DIR} ${AUX_DIR}/cgoto_pic.c)

#######################################################################################
# Others
#######################################################################################

MESSAGE("+++++  Checking other features")
CHECK_INCLUDE_FILE(dlfcn.h HAVE_DLFCN)
CHECK_INCLUDE_FILE(alloca.h HAVE_ALLOCA_H)
CHECK_FUNCTION_EXISTS(alloca HAVE_ALLOCA)
CHECK_INCLUDE_FILE(fcntl.h HAVE_FCNTL_H)
CHECK_FUNCTION_EXISTS(cygwin_conv_to_win32_path HAVE_CYGWIN_CONV_TO_WIN32_PATH)
CHECK_FUNCTION_EXISTS(drem HAVE_DREM)
CHECK_FUNCTION_EXISTS(dremf HAVE_DREMF)
CHECK_FUNCTION_EXISTS(dreml HAVE_DREML)
CHECK_FUNCTION_EXISTS(finite HAVE_FINITE)
CHECK_FUNCTION_EXISTS(finitef HAVE_FINITEF)
CHECK_FUNCTION_EXISTS(finitel HAVE_FINITEL)
CHECK_FUNCTION_EXISTS(isinf HAVE_ISINF)
CHECK_FUNCTION_EXISTS(isinff HAVE_ISINFF)
CHECK_FUNCTION_EXISTS(isinfl HAVE_ISINFL)
CHECK_FUNCTION_EXISTS(isnan HAVE_ISNAN)
CHECK_FUNCTION_EXISTS(isnanf HAVE_ISNANF)
CHECK_FUNCTION_EXISTS(isnanl HAVE_ISNANL)
CHECK_FUNCTION_EXISTS(trunc HAVE_TRUNC)
CHECK_FUNCTION_EXISTS(truncf HAVE_TRUNCF)
CHECK_FUNCTION_EXISTS(truncl HAVE_TRUNCL)
FIND_LIBRARY(HAVE_LIBDL dl)
FIND_LIBRARY(HAVE_LIBM dl)
FIND_LIBRARY(HAVE_LIBPTHREAD pthread)
CHECK_INCLUDE_FILE(pthread.h HAVE_PTHREAD_H)
CHECK_INCLUDE_FILE(tgmath.h HAVE_TGMATH_H)
CHECK_INCLUDE_FILE(unistd.h HAVE_UNISTD_H)
CHECK_INCLUDE_FILE(varargs.h HAVE_VARARGS_H)
CHECK_FUNCTION_EXISTS(_setjmp HAVE__SETJMP)
CHECK_FUNCTION_EXISTS(__sigsetjmp HAVE___SIGSETJMP)
CHECK_FUNCTION_EXISTS(getpagesize HAVE_GETPAGESIZE)
CHECK_INCLUDE_FILE(ieeefp.h HAVE_IEEEFP_H)
CHECK_FUNCTION_EXISTS(sigsetjmp HAVE_SIGSETJMP)
CHECK_INCLUDE_FILE(sys/cygwin.h HAVE_SYS_CYGWIN_H)
CHECK_INCLUDE_FILE(sys/mman.h HAVE_SYS_MMAN_H)
CHECK_INCLUDE_FILE(sys/stat.h HAVE_SYS_STAT_H)
CHECK_INCLUDE_FILE(sys/time.h HAVE_SYS_TIME_H)
CHECK_INCLUDE_FILE(sys/types.h HAVE_SYS_TYPES_H)