#!/bin/bash
#
# Copyright 2019 International Business Machines
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

# cxllib_handle_fault.sh
#
# These tests assume that user is root and memcpy afu is programmed.
# These tests also require the tool perf and the module cxl-memcpy.ko.

function usage
{
	echo 'cxllib_handle_fault.sh [-c <card_num>]' >&2
	exit 2
}

# Parse arguments
#
card=card0 # default
case $1 in
('')	;;
(-c)	card=card$2; shift 2;;
(*)	usage;;
esac
(( $# == 0 )) || usage

if [[ ! -d /sys/class/cxl/$card ]]
then
	echo cxllib_handle_fault.sh: $card: no such capi card
	exit 2
fi

# Load the kernel module if needed
#
if ! lsmod | grep cxl_memcpy >/dev/null 2>&1
then
	if ! insmod cxl-memcpy.ko
	then
		echo Please run 'KERNELDIR=<linux build tree> make cxl-memcpy.ko' >&2
		exit 1
	fi
fi

# Run the test
# memcpy #1 should trigger 10001 AFU originated pte misses
# memcpy #2 should trigger 1 pte miss (the master context setting)
#
export PATH=.:$PATH # give priority to local perf version

if perf stat -a -e cxl:cxl_pte_miss \
	memcpy_afu_ctx -p100 -l100 -r 2>&1 >/dev/null |
   grep ' 10,001      cxl:cxl_pte_miss' &&
   perf stat -a -e cxl:cxl_pte_miss \
	memcpy_afu_ctx -p100 -l100 -r -P 2>&1 >/dev/null |
   grep ' 1      cxl:cxl_pte_miss'
then
	echo cxllib_handle_fault.sh: test pass
	exit 0
fi

# Let us now lazily check what went wrong above

if ! which perf >/dev/null 2>&1
then
	echo perf: command not found >&2
	echo Please run 'KERNELDIR=<linux build tree> make perf'
	exit 2
fi

if ! perf stat -a -e cxl:cxl_pte_miss true 2>/dev/null
then
	echo 'perf: incompatible with current kernel' >&2
	[ -x perf ] && rm perf # remove local perf version
	echo Please run 'KERNELDIR=<linux build tree> make perf' >&2
	exit 3
fi

if ! [ -x memcpy_afu_ctx ]
then
	echo memcpy_afu_ctx: command not found >&2
	echo Please run make >&2
	exit 4
fi

if ! memcpy_afu_ctx
then
	echo memcpy_afu_ctx fails >&2
	echo Please check the AFU image >&2
	exit 5
fi

echo cxllib_handle_fault.sh: test fails for an unknown reason >&2
exit 6
