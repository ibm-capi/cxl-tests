cxl-tests
=========

`cxl-tests` is a test suite for the IBM Coherent Accelerator Processor
Interface.

`cxl-tests` provides tests for [`libcxl`](https://github.com/ibm-capi/libcxl)
and the CAPI `memcpy` AFU.

Requirements
------------

To compile, you'll need an appropriate GCC native or cross toolchain.

The version of `libcxl` to be tested must be in the `libcxl` directory. If the
directory is empty, the git submodule will be updated from GitHub.

To run the `libcxl` tests, you'll need a CAPI device. To run the `memcpy` AFU
tests, you'll need an appropriate CAPI FPGA with the `memcpy` AFU image flashed
on it.

Building
--------

To build, run `make`.

You can control the build of the tests through standard environment variables.
Default is 64-bit binaries, dynamic linking and same endianness as the build
machine.

If you're cross-compiling, set `CROSS_COMPILE` to the appropriate prefix.

For example, to cross-compile with `powerpc64le-linux-gnu-gcc`, statically
linked:

    $ export CROSS_COMPILE=powerpc64le-linux-gnu-
    $ export CFLAGS=-static
	$ make

Usage
-----

    $ export LD_LIBRARY_PATH=libcxl
    $ ./libcxl_tests        # Test libcxl
    $ ./memcpy_afu_ctx      # Test memcpy AFU memory copy
    $ ./memcpy_afu_ctx -t   # Test memcpy AFU timebase sync
    
    Usage: memcpy_afu_ctx [options]
    Options:
        -c <card_num>   Use this CAPI card (default 0).
        -h              Display this help text.
        -I <irq_count>  Define this number of interrupts (default 4).
        -i <irq_num>    Use this interrupt command source number (default 0).
        -k              Use the Stop_on_Invalid_Command and Restart logic.
        -l <loops>      Run this number of memcpy loops (default 1).
        -p <procs>      Fork this number of processes (default 1).
        -s <bufsize>    Copy this number of bytes (default 1024).
        -t              Do not memcpy. Test timebase sync instead.

Contributing
------------

Contributions are accepted via GitHub pull request.

All commit messages must include a `Signed-off-by:` line including your real
name, indicating that you have read and agree to the
[Developer Certificate of Origin v1.1](http://developercertificate.org).

Copyright
---------

Copyright &copy; 2017 International Business Machines Corporation

Licensed under the Apache License, Version 2.0 (the "License"); you may not use
this software except in compliance with the License. A copy of the License can
be found in the file `LICENSE`.

Unless required by applicable law or agreed to in writing, software distributed
under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
CONDITIONS OF ANY KIND, either express or implied.  See the License for the
specific language governing permissions and limitations under the License.
