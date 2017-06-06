#!/bin/bash

# cxl_eeh_tests.sh
#
# These tests assume that user is root and memcpy afu is programmed.

function usage
{
	echo 'cxl_eeh_tests.sh [-c <card_num>]' >&2
	exit 2
}
function set_timestamp
{
	timestamp="^\\$(dmesg | tail -1 | cut -d ] -f 1 | sed 's/\./\\./')]"
}
function dump_messages	# after timestamp
{
	dmesg | sed "1,/$timestamp/d"
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
	echo cxl_eeh_tests.sh: $card: no such capi card
	exit 2
fi

# Increase eeh max freeze default
echo 1000 > /sys/kernel/debug/powerpc/eeh_max_freezes || exit

# TEST1: Check cxl_reset
# Requires kernel >= 4.0.
#
echo cxl_eeh_tests.sh: resetting card
set_timestamp
card=/sys/class/cxl/$card
echo 0 > $card/perst_reloads_same_image
echo 1 > $card/reset || exit
lspci >/dev/null

# Wait for recovery notification
#
echo cxl_eeh_tests.sh: waiting for recovery
timeout=20 # seconds
while ((timeout-- >0))
do
	sleep 1
	dump_messages | grep 'EEH: Notify device driver to resume' && break
done >/dev/null
if ((timeout < 0))
then
	dmesg | tail
	echo cxl_eeh_test.sh: cxl_reset test fails
	exit 1
fi
echo cxl_eeh_tests.sh: driver notified

# Wait for device recovery
#
while ((timeout-- >0))
do
	sleep 1
	[ -d $card ] && break
done >/dev/null
if ((timeout < 0))
then
	echo cxl_eeh_test.sh: cxl_reset test fails
	exit 1
fi
echo cxl_eeh_tests.sh: device recovered

# Success
#
echo cxl_eeh_tests.sh: cxl_reset test passes

# Userspace tests
# TEST2: Check basic recovery, 1 process
#
domain=$(ls -l $card/device | cut -d/ -f9 | cut -d: -f1)
domain=/sys/kernel/debug/powerpc/PCI$domain

# You may need to adjust PATH and LD_LIBRARY_PATH.
#
public_dir=${PUBLIC_CXL_TESTS:-.}
PATH=$TEST_DIR:$public_dir:$PATH
LD_LIBRARY_PATH=$public_dir/libcxl:$LD_LIBRARY_PATH

# Launch a memcpy tester process.
#
set_timestamp
echo 1 > $card/perst_reloads_same_image
memcpy_afu_ctx -p1 -l1 >/tmp/recovery1.log 2>&1

# Inject error
#
echo cxl_eeh_tests.sh: injecting error
echo 1 > $domain/err_injct_outbound

# Wait for recovery. You should *not* see a message about waiting
# 5 seconds for complete hotplug.
#
echo cxl_eeh_tests.sh: waiting for recovery
timeout=5 # seconds
while ((timeout-- >0))
do
	sleep 1
	dump_messages | grep 'EEH: Sleep 5s ahead of complete hotplug' && break
done >/dev/null
if ((timeout >= 0))
then
	echo cxl_eeh_test.sh: err_injct test fails
	exit 1
fi

# Wait for device recovery
#
timeout=5 # seconds
while ((timeout-- >0))
do
	sleep 1
	[ -d $card ] && break
done >/dev/null
if ((timeout < 0))
then
	echo cxl_eeh_test.sh: err_inject test fails
	exit 1
fi

# The memcpy tester process should have been killed during recovery,
# so verify the card is back: this should report OK.
#
echo cxl_eeh_tests.sh: verifying memcpy afu
memcpy_afu_ctx -p1 -l1 >/tmp/recovery2.log 2>&1
if (($?))
then
	cat /tmp/recovery2.log
	echo cxl_eeh_test.sh: err_injct test fails for memcpy afu
	exit 1
fi

# If supported, then test timebase sync recovery.
#
read tb_synced </sys/class/cxl/card0/psl_timebase_synced
if ((tb_synced))
then
	echo cxl_eeh_tests.sh: verifying timebase sync
	memcpy_afu_ctx -p1 -l1 -t >/tmp/recovery3.log 2>&1
	if (($?))
	then
		cat /tmp/recovery3.log
		echo cxl_eeh_test.sh: err_injct test fails for timebase
		exit 1
	fi
fi

# Success
#
echo cxl_eeh_test.sh: err_injct test pass
exit 0
