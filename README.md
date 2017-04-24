		Automated testing of libcxl library
		-----------------------------------

LIBCXL
The libcxl to be tested must be in the 'libcxl' directory. If the directory is 
empty, the latest from github is used (https://github.com/ibm-capi/libcxl.git).
Building the tests requires:
  libcxl/libcxl.h
  libcxl/include/misc/cxl.h
  libcxl/libcxl.so
or
  libcxl/libcxl.a

TEST BUILD
You can control the build of the tests through standard environment variables. 
Default is 64-bit binaries, dynamic linking and same endianness as the build 
machine.
export CROSS_COMPILE=powerpc64le-linux-gnu-
export CFLAGS=-static
export BIT32=Y

TEST EXECUTION
$ export LD_LIBRARY_PATH=libcxl
$ ./libcxl_tests	# Test libcxl
$ ./memcpy_afu_ctx	# Test memcpy AFU memory copy
$ ./memcpy_afu_ctx -t	# test memcpy AFU timebase sync

Usage: memcpy_afu_ctx [options]
Options:
	-c <card_num>	Use this CAPI card (default 0).
	-h		Display this help text.
	-I <irq_count>	Define this number of interrupts (default 4).
	-i <irq_num>	Use this interrupt command source number (default 0).
	-k		Use the Stop_on_Invalid_Command and Restart logic.
	-l <loops>	Run this number of memcpy loops (default 1).
	-p <procs>	Fork this number of processes (default 1).
	-s <bufsize>	Copy this number of bytes (default 1024).
	-t		Do not memcpy. Test timebase sync instead.
