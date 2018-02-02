/*
 * Copyright 2017 International Business Machines
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define _DEFAULT_SOURCE
#define __STDC_FORMAT_MACROS
#define _ISOC11_SOURCE
#define _GNU_SOURCE

#include <sys/time.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <poll.h>
#include <endian.h>
#include <getopt.h>
#include <fcntl.h>
#include <errno.h>

#include <libcxl.h>
#include "memcpy_afu.h"

#define CACHELINESIZE	128

/* Queue sizes other than 512kB don't seem to work */
#define QUEUE_SIZE	4095*CACHELINESIZE

#define ERR_IRQTIMEOUT	0x1
#define ERR_EVENTFAIL	0x2
#define ERR_MEMCMP	0x4
#define ERR_INCR	0x8

/* Default amount of time to wait (in seconds) for a test to complete */
#define KILL_TIMEOUT	5
#define COMPLETION_TIMEOUT     120

static void get_name(char **name, int processes, int loops)
{
	if (asprintf(name, "memcpy_afu_ctx_poll(processes = %d, loops = %d)",
		     processes, loops) < 0) {
		perror("malloc");
		exit(1);
	}
}

struct memcpy_test_args {
	int processes;
	int loops;
	int buflen;
	int irq;
	int irq_count;
	int stop_flag;
	int timebase_flag;
	int increment_flag;
	int card;
	int completion_timeout;
	long int caia_major;
};

static int skip_process_element(struct memcpy_test_args *args, int pe)
{
	if (args->caia_major == 2) {
		switch (pe % 4) {
		case 0:	/* CT port */
		case 2: /* DMA port 1 */
		case 3: /* Reserved */
			return 1;
			break;
		case 1: /* DMA port 0 -- supported */
			break;
		}
	}
	return 0;
}

int set_afu_master_psa_registers(struct memcpy_test_args *args)
{
	struct cxl_afu_h *afu_master_h;
	struct cxl_ioctl_start_work *work;
	__be64 reg_data;
	char *cxldev;
	int process_element;
	int rc = 0;

	/* now that the AFU is started, lets set config options */
	if (asprintf(&cxldev, "/dev/cxl/afu%d.0m", args->card) < 0) {
		fprintf(stderr, "Out of memory\n");
		return 1;
	}

	do {
		afu_master_h = cxl_afu_open_dev(cxldev);
		if (afu_master_h == NULL) {
			fprintf(stderr, "Unable to open AFU Master cxl device %s: %d\n",
				cxldev, errno);
			free(cxldev);
			return 1;
		}
		process_element = cxl_afu_get_process_element(afu_master_h);
	} while (skip_process_element(args, process_element));

	work = cxl_work_alloc();
	if (work == NULL) {
		perror("cxl_work_alloc");
		return 1;
	}
	if (cxl_afu_attach_work(afu_master_h, work)) {
		perror("cxl_afu_attach_work(master)");
		rc = 1;
		goto err;
	}
	if (cxl_mmio_map(afu_master_h, CXL_MMIO_BIG_ENDIAN) == -1) {
		perror("Unable to map AFU Master problem state registers");
		rc = 1;
		goto err;
	}
	/* Set/Clear Bit 2 to stop AFU on Invalid Command */
	if (cxl_mmio_read64(afu_master_h, MEMCPY_AFU_PSA_REG_CFG, &reg_data) == -1) {
		perror("mmio read fail");
		rc = 1;
		goto err;
	}

	if (args->stop_flag)
		reg_data = reg_data | MEMCPY_AFU_PSA_REG_CFG_Stop_on_Inv_Cmd;
	else
		reg_data = reg_data & ~(MEMCPY_AFU_PSA_REG_CFG_Stop_on_Inv_Cmd);
	printf("# AFU PSA CFG REG: %#016llx\n", (unsigned long long)reg_data);
	if (cxl_mmio_write64(afu_master_h, MEMCPY_AFU_PSA_REG_CFG, reg_data) == -1) {
		perror("mmio write fail");
		rc = 1;
		goto err;
	}
err:
	cxl_afu_free(afu_master_h);
	cxl_work_free(work);
	free(cxldev);
	return rc;
}

#define CFG_TB_TICKS_PER_SEC 0x38

__u64 read_tb_ticks_per_sec()
{
	int fd;
	__u64 tb_ticks_per_sec;

	if ((fd = open("/proc/powerpc/systemcfg", O_RDONLY)) == -1) {
		perror("Unable to open /proc/powerpc/systemcfg");
		exit(1);
	}
	if (lseek(fd, CFG_TB_TICKS_PER_SEC, SEEK_SET) == -1) {
		perror("lseek");
		exit(1);
	}
	if (read(fd, &tb_ticks_per_sec, sizeof(tb_ticks_per_sec)) == -1) {
		perror("read");
		exit(1);
	}
	close(fd);
	return tb_ticks_per_sec;
}

#define SPRN_TBRL 0x10C
#define mftb() ({ \
	unsigned long rval; \
	asm volatile("mfspr %0,%1" : "=r" (rval) : "i" (SPRN_TBRL)); rval; \
	})

int test_afu_timebase(struct cxl_afu_h *afu_h, int count, __u64 ticks_per_sec)
{
	int i, j;
	long delta;
	__u64 afu_tb;

	if (count > 20)
		count = 20;
	for (i = 0; i < count; i++) {
		/* Request an update of the AFU TB */
		if (cxl_mmio_write64(afu_h, MEMCPY_PS_REG_TB, 0x0ULL) == -1)
			printf("# MMIO write to AFU TB register failed\n");

		/* Read the AFU TB, retry until non zero */
		j = 0;
		do {
			if (j++ > 10000000) {
				printf("# Timeout waiting for AFU TB update\n");
				return -1;
			}
			if (cxl_mmio_read64(afu_h, MEMCPY_PS_REG_TB,
					    &afu_tb) == -1)
				printf("# MMIO read from AFU TB register failed\n");
		} while (!afu_tb);

		/* Read the core timebase, compare */
		delta = mftb() - afu_tb;
		if (delta < 0)
			delta = -delta;
		delta = (delta * 1000000) / ticks_per_sec;
	        printf("# AFU: delta with core TB = %ld usecs\n", delta);
		if (delta > 16) {
			printf("# AFU: Error: delta with core TB > 16 usecs\n");
			return 1;
		}
		usleep(1000000);
	}
	return 0;
}

static void decode_we_status(int ret) {
	if (ret & MEMCPY_WE_STAT_TRANS_FAULT)
		fprintf(stderr, "Error: Translation Fault \"Continue\"\n");
	if (ret & MEMCPY_WE_STAT_AERROR)
		fprintf(stderr, "Error: Aerror\n");
	if (ret & MEMCPY_WE_STAT_DERROR)
		fprintf(stderr, "Error: Derror\n");
	if (ret & MEMCPY_WE_STAT_PSL_FAULT)
		fprintf(stderr, "Error: PSL Fault response\n");
	if (ret & MEMCPY_WE_STAT_INV_SRC)
		fprintf(stderr, "Error: Invalid interrupt source number\n");
	if (ret & MEMCPY_WE_STAT_PROC_TERM)
		fprintf(stderr, "Error: Process terminated\\n");
	if (ret & MEMCPY_WE_STAT_UNDEF_CMD)
		fprintf(stderr, "Error: Undefined Cmd or CAS_INV response\n");
}

int test_afu_memcpy(char *src, char *dst, size_t size, int count,
		    struct memcpy_test_args *args)
{
	struct cxl_afu_h *afu_h;
	struct cxl_ioctl_start_work *work;
	__u64 wed, status, process_handle_memcpy;

	int process_handle_ioctl;
	pid_t pid;
	int afu_fd, i, ret = 0, t;
	struct memcpy_weq weq;
	struct memcpy_work_element memcpy_we, irq_we, *queued_we;
	struct memcpy_work_element increment_we;
	struct cxl_event event;
	struct timeval timeout;
	struct timeval start, end, temp;
	fd_set set;
	char *cxldev;

	pid = getpid();
	if (asprintf(&cxldev, "/dev/cxl/afu%d.0s", args->card) < 0) {
		fprintf(stderr, "Out of memory\n");
		return 1;
	}

	do {
		afu_h = cxl_afu_open_dev(cxldev);
		if (afu_h == NULL) {
			fprintf(stderr, "Unable to open cxl device %s: %d\n",
				cxldev, errno);
			ret = 1;
			goto err1;
		}
		process_handle_ioctl = cxl_afu_get_process_element(afu_h);
	} while (skip_process_element(args, process_handle_ioctl));

	afu_fd = cxl_afu_fd(afu_h);
	memcpy_init_weq(&weq, QUEUE_SIZE);

	/* Point the work element descriptor (wed) at the weq */
	wed = MEMCPY_WED(weq.queue, QUEUE_SIZE/CACHELINESIZE);
	printf("# WED = 0x%llx for PID = %d via PE = %d\n",
               (unsigned long long)wed, pid, process_handle_ioctl);

	/* Setup the increment work element */
	increment_we.cmd = MEMCPY_WE_CMD(0, MEMCPY_WE_CMD_INCR);
	increment_we.status = 0;
	increment_we.length = htobe16((uint16_t)sizeof(pid_t));
	increment_we.src = htobe64((uintptr_t)src);
	increment_we.dst = htobe64((uintptr_t)dst);

	/* Setup the memcpy work element */
	memcpy_we.cmd = MEMCPY_WE_CMD(0, MEMCPY_WE_CMD_COPY);
	memcpy_we.status = 0;
	memcpy_we.length = htobe16((uint16_t)size);
	memcpy_we.src = htobe64((uintptr_t)src);
	memcpy_we.dst = htobe64((uintptr_t)dst);

	/* Setup the interrupt work element */
	irq_we.cmd = MEMCPY_WE_CMD(1, MEMCPY_WE_CMD_IRQ);
	irq_we.status = 0;
	irq_we.length = htobe16(args->irq);
	irq_we.src = 0;
	irq_we.dst = 0;

	/* Start the AFU */
	work = cxl_work_alloc();
	if (work == NULL) {
		perror("cxl_work_alloc");
		return 1;
	}
	if (cxl_work_set_wed(work, wed)) {
		perror("cxl_work_set_wed");
		return 1;
	}
	if (args->irq_count != -1) {
		if (cxl_work_set_num_irqs(work, args->irq_count)) {
			perror("cxl_work_set_num_irqs");
			return 1;
		}
	}
	ret = cxl_afu_attach_work(afu_h, work);
	if (ret) {
		perror("cxl_afu_attach_work(slave)");
		ret = 1;
		goto err2;
	}
	if (process_handle_ioctl < 0) {
		perror("process_handle_ioctl");
		ret = 1;
		goto err2;
	}
	if (cxl_mmio_map(afu_h, CXL_MMIO_BIG_ENDIAN) == -1) {
		perror("Unable to map problem state registers");
		ret = 1;
		goto err2;
	}
	if (args->timebase_flag)
		return test_afu_timebase(afu_h, count, read_tb_ticks_per_sec());

	if (cxl_mmio_read64(afu_h, MEMCPY_PS_REG_PH, &process_handle_memcpy) == -1) {
		perror("Unable to read mmaped space");
		ret = 1;
		goto err2;
	}
	process_handle_memcpy = process_handle_memcpy >> 48;

	if ((process_handle_memcpy == 0xdead) ||
	    (process_handle_memcpy == 0xffff))  {
		printf("# Bad process handle\n");
		ret = 1;
		goto err2;
	}
	assert(process_handle_memcpy == process_handle_ioctl);

	/* Initialise source buffer with unique(ish) per-process value */
	if (args->increment_flag) {
		*(pid_t *)src = htobe32(pid - 1);
	} else {
		for (i = 0; i < size; i++)
			*(src + i) = pid & 0xff;
	}

	FD_ZERO(&set);
	FD_SET(afu_fd, &set);
	gettimeofday(&start, NULL);

	for (i = 0; i < count; i++) {
		ret = 0;
		if (args->increment_flag){
			*(pid_t *)src = htobe32(be32toh(*(pid_t *)src) + 1);
			queued_we = memcpy_add_we(&weq, increment_we);
		} else
			queued_we = memcpy_add_we(&weq, memcpy_we);
		if (args->irq)
			memcpy_add_we(&weq, irq_we);
		queued_we->cmd |= MEMCPY_WE_CMD_VALID;

		/* If stop flag set, need to restart this CTX in the MCP AFU */
		if (args->stop_flag)
			if (cxl_mmio_write64(afu_h, MEMCPY_PS_REG_PCTRL,
					     MEMCPY_PS_REG_PCTRL_Restart) == -1) {
				ret = 1;
				goto err2;
			}

		if (args->irq) {
			/* Set timeout to 1 second */
			timeout.tv_sec = 1;
			timeout.tv_usec = 0;
			if (select(afu_fd+1, &set, NULL, NULL, &timeout) <= 0) {
				printf("#\tTimeout waiting for interrupt! loop: %i pe: %i\n", i, process_handle_ioctl);
				ret |= ERR_IRQTIMEOUT;
			} else {
				if (cxl_read_expected_event(afu_h, &event,
							    CXL_EVENT_AFU_INTERRUPT, args->irq)) {
					printf("# Failed reading expected event\n");
					ret |= ERR_EVENTFAIL;
				}
			}
			do {
				/* Make sure AFU is waiting on restart */
				cxl_mmio_read64(afu_h, MEMCPY_PS_REG_STATUS,
						&status);
			} while (!(status & MEMCPY_PS_REG_STATUS_Stopped));
			/* do restart */
			cxl_mmio_write64(afu_h, MEMCPY_PS_REG_PCTRL,
					 MEMCPY_PS_REG_PCTRL_Restart);
		}

		/* We have to do this even for the interrupt driven case because we need
		 * to wait for this flag before setting the completion bit. */
		gettimeofday(&timeout, NULL); /* reuse timeout */
		temp.tv_sec = args->completion_timeout;
		temp.tv_usec = 0;
		timeradd(&timeout, &temp, &timeout);
		for (;; gettimeofday(&temp, NULL)) {
			if (timercmp(&temp, &timeout, >)) {
				printf("# Timeout polling for completion\n");
				break;
			}

			if (queued_we->status) {
				if (ret != MEMCPY_WE_STAT_COMPLETE)
					decode_we_status(ret);
				break;
			}
		}
		if (args->increment_flag) {
			printf("src=%u dst=%u\n", be32toh(*(pid_t *)src),
			        be32toh(*(pid_t *)dst));
			ret |= be32toh(*(pid_t *)dst)
			     - be32toh(*(pid_t *)src) == 1 ? 0 : ERR_INCR;
		} else {
			ret |= memcmp(dst, src, size) == 0 ? 0 : ERR_MEMCMP;
		}
		if (ret) {
			printf("# Error on loop %d\n", i);
			break;
		}
		memset(dst, 0, size);
	}

	gettimeofday(&end, NULL);
	t = (end.tv_sec - start.tv_sec)*1000000 + end.tv_usec - start.tv_usec;
	printf("# %d loops in %d uS (%0.2f uS per loop)\n", count, t, ((float) t)/count);

err2:
	cxl_afu_free(afu_h);
err1:
	free(cxldev);
	return ret;
}

static int get_caia_major(struct memcpy_test_args *args)
{
	struct cxl_adapter_h *adapter;
	char *name;
	long caia_minor;
	int card_num;

	/* get caia version of adapter */
	cxl_for_each_adapter(adapter) {
		name = cxl_adapter_dev_name(adapter);
		sscanf(name, "card%d", &card_num);
		if (card_num == args->card) {
			if (cxl_get_caia_version(adapter, &args->caia_major,
						 &caia_minor)) {
				perror("cxl_get_caia_version");
				return 1;
			}
			break;
		}
	}
	if (errno) {
		perror("cxl_for_each_adapter");
		return 1;
	}
	if (card_num != args->card) {
		fprintf(stderr, "/sys/class/cxl/card%d: no such cxl card\n",
			args->card);
		return 1;
	}
	return 0;
}

int run_tests(void *argp)
{
	struct memcpy_test_args *args = argp;
	int processes = args->processes;
	int loops = args->loops;
	int buflen = args->buflen;
	int i, j;
	char *src, *dst;
	pid_t pid;

	if (get_caia_major(args))
		return 1;

	if (set_afu_master_psa_registers(args))
		return 1;

	/* Allocate memory areas for afu to copy to/from */
	src = aligned_alloc(CACHELINESIZE, buflen);
	dst = aligned_alloc(CACHELINESIZE, buflen);

	printf("# Starting %d processes doing %d %s loops\n", processes, loops,
	       args->increment_flag ? "increment" : "memcpy");
	printf("# Queue size: %dkB, Queue length: %d\n", QUEUE_SIZE/1024,
	       memcpy_queue_length(QUEUE_SIZE));
	printf("# src: %p dst: %p\n", src, dst);
	for (i = 0; i < processes; i++) {
		if (!fork())
			/* Child process */
			exit(test_afu_memcpy(src, dst, buflen, loops, args));
	}

	for (i = 0; i < processes; i++) {
		pid = wait(&j);
		if (pid && j) {
			printf("# Error copying for PID = %d\n", pid);
			return 1;
		}
	}

	return 0;
}

static void usage()
{
	fprintf(stderr, "Usage: memcpy_afu_ctx [options]\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "\t-a\t\tAdd 1. Do not memcpy. Test increment instead.\n");
	fprintf(stderr, "\t-c <card_num>\tUse this CAPI card (default 0).\n");
	fprintf(stderr, "\t-e <timeout>\tEnd timeout.\n"
			"\t\t\tSeconds to wait for the AFU to signal completion.\n");
	fprintf(stderr, "\t-h\t\tDisplay this help text.\n");
	fprintf(stderr,
	        "\t-I <irq_count>\tDefine this number of interrupts (default 4).\n");
	fprintf(stderr,
	        "\t-i <irq_num>\tUse this interrupt command source number (default 0).\n");
	fprintf(stderr,
	        "\t-k\t\tUse the Stop_on_Invalid_Command and Restart logic.\n");
	fprintf(stderr,
	        "\t-l <loops>\tRun this number of memcpy loops (default 1).\n");
	fprintf(stderr,
	        "\t-p <procs>\tFork this number of processes (default 1).\n");
	fprintf(stderr,
	        "\t-s <bufsize>\tCopy this number of bytes (default 1024).\n");
	fprintf(stderr, "\t-t\t\tTimebase. Do not memcpy. Test timebase sync instead.\n");
	exit(2);
}

int main(int argc, char *argv[])
{
	int c, rc;
	char *name;
	struct memcpy_test_args args = {
		.processes = 1,
		.loops = 1,
		.buflen = 1024,
		.irq = 0,
		.irq_count = -1,
		.stop_flag = 0,
		.timebase_flag = 0,
		.increment_flag = 0,
		.card = 0,
		.completion_timeout = COMPLETION_TIMEOUT,
		.caia_major = 0,
	};

	while (1) {
		c = getopt(argc, argv, "+ahktp:l:s:i:I:c:e:");
		if (c < 0)
			break;
		switch (c) {
		case '?':
		case 'h':
			usage();
			break;
		case 'a':
			args.increment_flag = 1;
			break;
		case 'k':
			/* This arg is to change the behavior of MCP.
			 * Rather than poll work valid work in the WEQ,
			 * you need to "restart" the state machine to
			 * tell it has work */
			args.stop_flag = 1;
			break;
		case 't':
			/* Test timebase sync */
			args.timebase_flag = 1;
			break;
		case 'p':
			args.processes = atoi(optarg);
			break;
		case 'l':
			args.loops = atoi(optarg);
			break;
		case 's':
			args.buflen = atoi(optarg);
			break;
		case 'i':
			args.irq = atoi(optarg);
			break;
		case 'I':
			args.irq_count = atoi(optarg);
			break;
		case 'c':
			args.card = atoi(optarg);
			break;
		case 'e':
			/* end timeout */
			args.completion_timeout = atoi(optarg);
			break;
		}
	}
	if (argv[optind]) {
		fprintf(stderr,
			"Error: Unexpected argument '%s'\n", argv[optind]);
		usage();
	}
	get_name(&name, args.processes, args.loops);
	printf("1..1\n");
	printf("# test: %s\n", name);
	rc = run_tests((void *) &args);
	free(name);
	return rc;
}
