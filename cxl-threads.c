/*
 * Copyright 2018 International Business Machines
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

/*
 * **************************************************
 *             CXL Threads Test Case
 * **************************************************
 * This code tests the scenario where in call to the cxl_afu_attach
 * and mmio access to the problem state area happens on different
 * threads. The thread calling the cxl_afu_attach function exits
 * after attaching the process element to the afu handle. Other
 * thread (slave thread) go in to a loop copying random data (1K)
 * from a source to destination buffer using implemented function
 * afu_memcpy.
 *
 * Usage:
 *  cxl-threads [options]
 *  -n: Number of threads that execute memcpy.
 *  -t: if set the work element setup is done on the main thread.
 *  -c: Card index to runs tests on.
 *  -l: Number of loops that each thread executes
 *  -x: Exit the main thread after spawing the child threads.
 *  -z: Exit the main thread after spawing the child threads(zombie-state)
 *  -j: Join the spawned thread to exit (default)
 *  -d: Detach from the child threads and die (dead-state)
 *  -m: Use malloced memory instead of static memory for src/dst buffer
 *  -s: Size of the copy buffer used.
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <endian.h>
#include <err.h>
#include <pthread.h>
#include <libcxl.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/limits.h>
#include <fcntl.h>
#include <errno.h>
#include <getopt.h>
#include <sys/stat.h>
#include <syscall.h>
#include "memcpy_afu.h"

#define ARRAY_SIZE(__arr__)  (sizeof(__arr__)/sizeof(__arr__)[0])

/* default Master context to be used */
#define MEMCPY_MASTER_CONTEXT "/dev/cxl/afu0.0m"

#define MAX_BUFFER_SIZE  1024

/* holds the path to  slave context to be used */
char arg_master_context[PATH_MAX] = MEMCPY_MASTER_CONTEXT;

/* is psa setup done a separate thread */
int setup_on_thread = 1;

/* number of slave threads to spawn */
int num_threads = 2;

/* number of memcpy loops each slave threads to does */
int num_loops = 1;

/* use dynamically allocated memory */
int use_malloc = 1;

/* Buffer size to use */
size_t szbuffer = 128;

/* main thread state after spawing the child threads*/
enum {
	EXIT_JOIN,
	EXIT_DIE,
	EXIT_ZOMBIE
} exit_after_spawn = EXIT_JOIN;

/* afu work element queue */
struct memcpy_weq weq;

/* Per process slave context */
struct cxl_afu_h *afu_h;

/* Ask afu to perform a strcpy operation and wait for the operation to finish */
static int afu_memcpy(char *dst, char *src, size_t size);

/* Sets up per process problem state area */
static int setup_afu_slave_psa(struct cxl_afu_h *afu);
static void *setup_afu_slave_psa_proc(void *afu);

/* protects the work queue from concurrent modifications*/
static pthread_mutex_t mtx_memcpy = PTHREAD_MUTEX_INITIALIZER;

/* amount of time used for each poll iteration */
static const struct timespec poll_time = {
	.tv_sec = 0,
	.tv_nsec = 1000 /* 1 usec interval */
};

/* Array to pthread handles */
#define MAX_NUM_THREADS 32
pthread_t arr_threads[MAX_NUM_THREADS];

void dumpbuffer(char *bfr, size_t size)
{
	size_t count;
	for(count = 0; count < size; ++count)
	{
		if (count % 8 == 0)
			printf("\n%04o: ", (int)count);
		printf("%2X ", bfr[count]);
	}
	printf("\n");
}

void *afu_slave_threadproc_dynamic(void *arg)
{
	int thindex, index;
	uintptr_t rc = 0;
	char *srcbuffer = NULL, *dstbuffer = NULL;
	int fd_random;
	int loops = (uintptr_t)arg;

	/* get the task_struct pid */
	thindex = syscall(SYS_gettid);

	fd_random = open("/dev/urandom", O_RDONLY);
	if (fd_random < 0) {
		perror("Unable to open random source /dev/urandom");
		rc = 1;
		goto out;
	}

	printf("THREAD[%d]: Starting with loop count %d\n", thindex, loops);
	if (exit_after_spawn) {
		printf("THREAD[%d]: Sleeping for some time\n", thindex);
		sleep(5);
	}

	/* All set now perform memcpy using a poll loop */
	for (index = 0; index < loops; ++index) {
		int ret;

		srcbuffer = aligned_alloc(128, szbuffer);
		dstbuffer = aligned_alloc(128, szbuffer);

		if ((srcbuffer == NULL) || (dstbuffer == NULL)) {
		  printf("THREAD[%d]: Copy Loop index %d .. "
			 "Unable to allocate memory\n", thindex, index);
		  goto loopend;
		}

		bzero(dstbuffer, szbuffer);
		ret = read(fd_random, srcbuffer, szbuffer);
		if (ret < szbuffer) {
			rc = errno;
			printf("THREAD[%d]: Copy Loop index %d .. "
			       "Unable to populate srcbuffer Error=%d\n", thindex,
			       index, (int)rc);
			goto loopend;
		}

		ret = afu_memcpy(dstbuffer, srcbuffer, szbuffer);
		if (ret) {
			rc = ret;
			perror("Unable to perform memcpy");
			goto out;
		}

		/* compare the strings */
		ret = memcmp(srcbuffer, dstbuffer, szbuffer);
		if (ret) {
			size_t mindex;
			rc = ret;
			printf("THREAD[%d]: Copy Loop index %d .. "
			       "ERROR[rc=%lu]\n", thindex, index, rc);
			for (mindex = 0; mindex < szbuffer; ++mindex)
				if (srcbuffer[mindex] != dstbuffer[mindex]) {
					printf("THREAD[%d]: Mismatch at index=%lu %x!=%x\n",
					       thindex, mindex, srcbuffer[mindex],
						dstbuffer[mindex]);
					break;
				}
			printf("THREAD[%d]: SrcBuffer = %p\n", thindex, srcbuffer);
			dumpbuffer(srcbuffer, szbuffer);
			printf("THREAD[%d]: DstBuffer = %p\n", thindex, dstbuffer);
			dumpbuffer(dstbuffer, szbuffer);
		} else {
			printf("THREAD[%d]: Copy Loop index %d..OK\n",
			       thindex, index);
		}
loopend:
		free(srcbuffer); srcbuffer = NULL;
		free(dstbuffer); dstbuffer = NULL;
	}

out:
	free(srcbuffer);
	free(dstbuffer);

	if (fd_random >= 0)
		close(fd_random);
	return ((void *)rc);
}

void *afu_slave_threadproc_static(void *arg)
{
	int thindex, index;
	uintptr_t rc = 0;
	int fd_random;
	int loops = (uintptr_t)arg;
	char srcbuffer[MAX_BUFFER_SIZE] __attribute__((aligned (128)));
	char dstbuffer[MAX_BUFFER_SIZE] __attribute__((aligned (128)));

	/* get the task_struct pid */
	thindex = syscall(SYS_gettid);

	fd_random = open("/dev/urandom", O_RDONLY);
	if (fd_random < 0) {
		perror("Unable to open random source /dev/urandom");
		rc = 1;
		goto out;
	}

	printf("THREAD[%d]: Starting with loop count %d\n", thindex, loops);
	if (exit_after_spawn) {
		printf("THREAD[%d]: Sleeping for some time\n", thindex);
		sleep(5);
	}

	/* All set now perform memcpy using a poll loop */
	for (index = 0; index < loops; ++index) {
		int ret;

		bzero(dstbuffer, szbuffer);
		ret = read(fd_random, srcbuffer, szbuffer);
		if (ret < szbuffer) {
			rc = errno;
			printf("THREAD[%d]: Copy Loop index %d .. "
			       "Unable to populate srcbuffer Error=%d\n", thindex,
			       index, (int)rc);
			continue;
		}

		ret = afu_memcpy(dstbuffer, srcbuffer, szbuffer);
		if (ret) {
			rc = ret;
			perror("Unable to perform memcpy");
			goto out;
		}

		/* compare the strings */
		ret = memcmp(srcbuffer, dstbuffer, szbuffer);
		if (ret) {
			size_t mindex;
			rc = ret;
			printf("THREAD[%d]: Copy Loop index %d .. "
			       "ERROR[rc=%lu]\n", thindex, index, rc);
			for (mindex = 0; mindex < szbuffer; ++mindex)
				if (srcbuffer[mindex] != dstbuffer[mindex]) {
					printf("THREAD[%d]: Mismatch at index=%lu %x!=%x\n",
					       thindex, mindex, srcbuffer[mindex],
						dstbuffer[mindex]);
					break;
				}
			printf("THREAD[%d]: SrcBuffer = %p\n", thindex, srcbuffer);
			dumpbuffer(srcbuffer, szbuffer);
			printf("THREAD[%d]: DstBuffer = %p\n", thindex, dstbuffer);
			dumpbuffer(dstbuffer, szbuffer);
		} else {
			printf("THREAD[%d]: Copy Loop index %d..OK\n",
			       thindex, index);
		}
	}

out:
	if (fd_random >= 0)
		close(fd_random);
	return ((void *)rc);
}


/* Code entry point */
int main(int argc, char *argv[])
{
	struct cxl_afu_h *afu = NULL;
	void *ret = NULL;
	int rc = 1, index, c;
	pthread_t th_setup;
	int delay = 0;
	void *(*threadproc)(void *) = afu_slave_threadproc_static;

	while ((c = getopt(argc, argv, "n:tc:hl:zjdms:")) > 0) {
		switch (c) {
		case 's':
			szbuffer = atol(optarg);
			if (szbuffer > MAX_BUFFER_SIZE) {
				warnx("[ERROR] Invalid buffersize %lu."
				      " Max supported=%d", szbuffer,
				      MAX_BUFFER_SIZE);
				goto out;
			}
		case 'm': /* use dynamic memory */
			use_malloc = 1;
			threadproc = afu_slave_threadproc_dynamic;
			break;
		case 'n': /* number of slave thread */
			num_threads = atoi(optarg);
			if (num_threads <= 0 ||
			    (num_threads > ARRAY_SIZE(arr_threads))) {
				warnx("[ERROR] Invalid number threads %d."
				      " Max supported=%lu", num_threads,
				      ARRAY_SIZE(arr_threads));
				goto out;
			}
			break;
		case 'l': /* number of loops */
			num_loops = atoi(optarg);
			break;
		case 't': /* setup slave psa on a separate thread */
			setup_on_thread = 0;
			break;
		case 'z': /* become a zombie after spawing threads */
			exit_after_spawn = EXIT_ZOMBIE;
			break;
		case 'd': /* detach and die after spawing threads */
			exit_after_spawn = EXIT_DIE;
			break;
		case 'j': /* join the child thread after spawing*/
			exit_after_spawn = EXIT_JOIN;
			break;

		case 'c': /* target a specific card */
			if (optarg == NULL) {
				warnx("Missing Argument for card index");
				return 1;
			}
			snprintf(arg_master_context,
				 sizeof(arg_master_context),
				 "/dev/cxl/afu%d.0m", atoi(optarg));
			arg_master_context[sizeof(arg_master_context) - 1] = 0;
			break;
		case '?':
			warnx("Invalid argument '%c'", optopt);
		case 'h':
			fprintf(stderr, "Usage:%s [-n <num-threads>]"
			      " [-c <card-index>]"
			      " [-t]\n", argv[0]);
			fprintf(stderr, "-n: Number of threads that execute "
				"memcpy.\n");
			fprintf(stderr, "-t: if set the work element setup"
				" is done on the main thread\n");
			fprintf(stderr, "-c: Card index to runs tests on.\n");
			fprintf(stderr, "-z: Exit the main thread after spawing"
				" the child threads(zombie-state)\n");
			fprintf(stderr, "-j: Join the spawned thread to exit"
				"(default)\n");
			fprintf(stderr, "-d: Detach from the child threads and die"
				" (dead-state)\n");
			fprintf(stderr, "-m: Use malloced memory instead of static"
				" memory for src/dst buffer\n");
			fprintf(stderr, "-s: Size of the copy buffer used.\n");
			return ((c == 'h') ? 0 : 1);
		}
	}

	printf("INFO: Will use buffer size=%lu\n", szbuffer);
	printf("INFO: Will use %s memory\n", use_malloc ? "malloced" : "static");

	/* Lookup if we have an afu configured */
	printf("INFO: Opening Master context %s..", arg_master_context);

	afu = cxl_afu_open_dev(arg_master_context);
	if (afu == NULL) {
		printf("ERROR\n");
		goto out;
	} else {
		printf("done\n");
	}

	/* ********************* Setup Phase ******************* */
	/* Setup the slave afu context */
	if (setup_on_thread) {
		printf("INFO: Set up slave psa area on a separate thread..");
		rc = pthread_create(&th_setup, NULL, &setup_afu_slave_psa_proc,
				    (void *)afu);
		if (!rc) {
			pthread_join(th_setup, &ret);
			rc = (uintptr_t)(ret);
		}
	} else {
		printf("INFO: Setting up slave psa area on a main thread..");
		rc = setup_afu_slave_psa(afu);
	}

	/* check for error */
	if (rc) {
		perror("Unable to setup slave psa");
		goto out;
	}
	printf("done\n");

	/* *************** Computation Phase ***************** */
	printf("INFO: Creating %d slave threads\n", num_threads);
	if (num_loops > 0)
		printf("INFO: Number of loops per thread = %d\n", num_loops);
	else
		printf("INFO: Duration between exit of each = %d\n", num_loops);

	for (index = 0; index < num_threads; ++index) {
		/*
		 * if num_loops is nagtive we need to dynamically
		 * calculate number of loops
		 */
		delay = (num_loops > 0) ? num_loops :
			-((index + 1) * num_loops);

		rc = pthread_create(arr_threads + index, NULL,
				    threadproc,
				    (void *)((uintptr_t) delay));
		if (rc) {
			warnx("[ERROR] Unable to create thread index %d: %s\n",
			      index, strerror(rc));
			exit(1);
		}
	}

	if (exit_after_spawn == EXIT_ZOMBIE) {
		printf("INFO: Main thread exiting\n");
		pthread_exit(NULL);
		return 0; /* should never happen */
	} else if (exit_after_spawn == EXIT_DIE) {
		printf("INFO: Detaching all threads\n");
		for (index = 0; index < num_threads; ++index)
			pthread_detach(arr_threads[index]);
		printf("INFO: Main thread exiting\n");
		pthread_exit(NULL);
		exit(0); /* should never happen */
	} else {
		printf("INFO: Waiting for all threads to exit\n");
		for (index = 0; index < num_threads; ++index) {
			pthread_join(arr_threads[index], &ret);
			if (ret != NULL) {
				warnx("[ERROR] Thread index %d error %lu",
				      index, ((uintptr_t)ret));
				rc = ((uintptr_t)ret);
			}
		}
	}

out:
	/* ********** Deinitialization Phase ************ */
	if (afu_h)
		cxl_afu_free(afu_h);
	if (afu)
		cxl_afu_free(afu);

	return rc;
}


/* Ask afu to perform a strcpy operation and wait for the operation to finish */
int afu_memcpy(char *dst, char *src, size_t size)
{
	struct memcpy_work_element memcpy_we, *queued_we;
	int ret = 0;
	struct timespec rem;

	/* Setup a work element in the queue */
	memcpy_we.cmd = MEMCPY_WE_CMD(0, MEMCPY_WE_CMD_COPY);
	memcpy_we.status = 0;
	memcpy_we.length = htobe16((uint16_t)size);
	memcpy_we.src = htobe64((uintptr_t)src);
	memcpy_we.dst = htobe64((uintptr_t)dst);

retry:
	ret = pthread_mutex_lock(&mtx_memcpy);
	if (ret) {
		sleep(1);
		goto retry;
	}
	queued_we = memcpy_add_we(&weq, memcpy_we);
	queued_we->cmd |= MEMCPY_WE_CMD_VALID;
	pthread_mutex_unlock(&mtx_memcpy);

	/* We poll the status of the work element */
	while (queued_we->status == 0)
		nanosleep(&poll_time, &rem);

	ret = queued_we->status;
	return ret == MEMCPY_WE_STAT_COMPLETE ? 0 : ret;
}

#define CACHELINESIZE	128
#define QUEUE_SIZE	2

void *setup_afu_slave_psa_proc(void *afu)
{
	uintptr_t ret = setup_afu_slave_psa((struct cxl_afu_h *)afu);

	return (void *)(ret);
}

/*
 * Sets up the afu per process context. This allocates the work
 * element queue and validates if the process element is valid.
 */
int setup_afu_slave_psa(struct cxl_afu_h *afu)
{

	int ret;
	__u64 wed, process_handle_memcpy, process_handle_ioctl;

	/* Get handle to Per Process PSA context */
	afu_h = cxl_afu_open_h(afu, CXL_VIEW_SLAVE);
	if (afu_h == NULL) {
		perror("Unable to open AFU Slave cxl device");
		return 1;
	}

	/* initialize the work queue */
	memcpy_init_weq(&weq, QUEUE_SIZE * CACHELINESIZE);

	/* Point the work element descriptor (wed) at the weq */
	wed = MEMCPY_WED(weq.queue, QUEUE_SIZE);

	/* attach the wed to the context */
	ret = cxl_afu_attach(afu_h, wed);
	if (ret) {
		perror("Unable to attach the slave cxl context");
		return 1;
	}

	/* Map the per process psa to an application vma */
	ret = cxl_mmio_map(afu_h, CXL_MMIO_BIG_ENDIAN);
	if (cxl_mmio_map(afu_h, CXL_MMIO_BIG_ENDIAN) == -1) {
		perror("Unable to map problem state registers");
		return 1;
	}

	/* Fetch the process element from kernel */
	process_handle_ioctl = cxl_afu_get_process_element(afu_h);
	if (process_handle_ioctl < 0) {
		perror("process_handle_ioctl");
		return 1;
	}

	/* read the process element handle from the PPSA */
	if (cxl_mmio_read64(afu_h, MEMCPY_PS_REG_PH,
			    &process_handle_memcpy) == -1) {
		perror("Unable to read mmaped space");
		return 1;
	}

	process_handle_memcpy = process_handle_memcpy >> 48;

	/* See if the process element is valid */
	if ((process_handle_memcpy == 0xdead) ||
	    (process_handle_memcpy == 0xffff))  {
		warnx("Bad process handle");
		return 1;
	}
	assert(process_handle_memcpy == process_handle_ioctl);

	/* restart the slice */
	cxl_mmio_write64(afu_h, MEMCPY_PS_REG_PCTRL,
			 0x8000000000000000ULL);

	return 0;
}
