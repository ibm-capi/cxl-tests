#
# Copyright 2017 International Business Machines
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

srcdir= $(PWD)
ifeq ($(KERNELRELEASE),)
include Makefile.vars

libcxl_dir = libcxl

CFLAGS += -I $(libcxl_dir) -I $(libcxl_dir)/include
LDFLAGS += -L $(libcxl_dir) -lcxl -lpthread

# Add tests here
tests = memcpy_afu_ctx.c libcxl_tests.c cxl-threads.c

# Add any .o files tests may depend on
test_deps = memcpy_afu.o

# kernel module
kmodule = cxl-memcpy.ko

tests: all
all: $(tests:.c=)

libcxl_objs = libcxl.a libcxl.so
libcxl_deps = $(foreach dep, $(libcxl_objs), $(libcxl_dir)/$(dep))

-include $(tests:.c=.d)
-include $(test_deps:.o=.d)
-include $(kmodule:.ko=.d)
include Makefile.rules

$(libcxl_dir)/Makefile:
	$(call Q, GIT submodule init, git submodule init)
	$(call Q, GIT submodule update, git submodule update)

$(libcxl_deps): $(libcxl_dir)/Makefile
	@mkdir -p $(libcxl_dir)/include/misc # temporary workaround
	$(MAKE) -C $(libcxl_dir) all

$(test_deps):

ifeq ($(KERNELDIR),)
perf:
	@echo Please set "'KERNELDIR=<linux build tree>'"; false
else
perf:
	make -C $(KERNELDIR)/tools/perf
	cp $(KERNELDIR)/tools/perf/perf .
endif

KERNELDIR ?= /lib/modules/$(shell uname -r)/build

%.ko : %.c
	$(call Q,CC, $(CC) -MM $(CFLAGS) $< > $*.d, $*.d)
	$(call Q,SED, sed -i -e "s/$@.o/$@/" $*.d, $*.d)
	$(MAKE) -C $(KERNELDIR) M=$(shell pwd) modules

%.o : %.S
	$(call Q,CC, $(CC) -MM $(CFLAGS) $< > $*.d, $*.d)
	$(call Q,SED, sed -i -e "s/$@.o/$@/" $*.d, $*.d)
	$(call Q,CC, $(CC) $(CFLAGS) -c $<, $<)

% : %.c $(libcxl_deps) $(test_deps)
	$(call Q,CC, $(CC) -MM $(CFLAGS) $< > $*.d, $*.d)
	$(call Q,SED, sed -i -e "s/$@.o/$@/" $*.d, $*.d)
	$(call Q,CC, $(CC) $(CFLAGS) $(filter %.c %.o,$^) $(LDFLAGS) -o $@, $@)

clean:
	/bin/rm -f $(tests:.c=) $(patsubst %.c,%.d,$(tests)) $(test_deps) \
		$(patsubst %.o,%.d,$(test_deps)) $(kmodule:.ko=.d) perf
	$(MAKE) -C $(KERNELDIR) M=$(shell pwd) clean
	$(MAKE) -C $(libcxl_dir) clean

.PHONY: clean all tests

else
ccflags-y += -I$(srcdir)/$(public_dir)
obj-m := cxl-memcpy.o
endif
