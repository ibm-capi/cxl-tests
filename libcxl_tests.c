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

#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include <libcxl.h>
#include "memcpy_afu.h"

#define CACHELINESIZE   128

/* Queue sizes other than 512kB don't seem to work */
#define QUEUE_SIZE      4095*CACHELINESIZE

static int isRadix;

static int set_isRadix(void)
{
	const char line[] = "MMU\t\t: Radix\n";
	char buffer[1024]; /* Assuming max line length of 1024 chars */
	FILE *fp;
	int ret = 0;

	fp = fopen("/proc/cpuinfo", "rt");

	if (fp == NULL) {
		perror("Unable to open /proc/cpuinfo");
		return -1;
	}

	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		if (strncmp(line, buffer, sizeof(buffer)) == 0) {
			isRadix = 1;
			break;
		}
	}

	if (ferror(fp)) {
		perror("Unable to read contents of /proc/cpuinfo");
		ret = -1;
	}
	fclose(fp);
	return ret;
}

int attach_afu(struct cxl_afu_h *afu)
{
	int ret = 0;
	struct cxl_ioctl_start_work *work;

	work = cxl_work_alloc();
	if (work == NULL) {
		perror("cxl_work_alloc");
		return 1;
	} 
	ret = cxl_afu_attach_work(afu, work);
	if (ret)
		perror("cxl_afu_attach_work");
	return ret;
}

void pr_cxl_sysfs(struct cxl_afu_h *afu_h)
{
	char *buf;

	if (cxl_afu_sysfs_pci(afu_h, &buf) == -1) {
		perror("cxl_afu_sysfs_pci");
		exit(1);
	}
	printf("  PCI sysfs directory: %s\n", buf);
	free(buf);
}

void pr_adapter_attr(struct cxl_adapter_h *adapter)
{
	long major, minor;
	enum cxl_image image;
	char *name = cxl_adapter_dev_name(adapter);

	if (cxl_get_base_image(adapter, &major)) {
		perror("cxl_get_base_image");
		exit(1);
	}
	printf("    %s.base_image=%ld\n", name, major);
	if (cxl_get_caia_version(adapter, &major, &minor)) {
		perror("cxl_get_caia_version");
		exit(1);
	}
	printf("    %s.caia_version.major=%ld\n", name, major);
	printf("    %s.caia_version.minor=%ld\n", name, minor);
	if (cxl_get_image_loaded(adapter, &image)) {
		perror("cxl_get_image_loaded");
		exit(1);
	}
	switch (image) {
	case CXL_IMAGE_FACTORY:
		printf("    %s.image_loaded=factory\n", name); break;
	case CXL_IMAGE_USER:
		printf("    %s.image_loaded=user\n", name); break;
	}
	if (cxl_get_psl_revision(adapter, &major)) {
		perror("cxl_get_psl_revision");
		exit(1);
	}
	printf("    %s.psl_revision=%ld\n", name, major);
	/* The following attribute appeared in Linux v4.6 */
	if (cxl_get_psl_timebase_synced(adapter, &major))
		perror("cxl_get_psl_timebase_synced");
	else
		printf("    %s.psl_timebase_synced=%ld\n", name, major);
	/* POWER9 specific attribute */
	if (cxl_get_tunneled_ops_supported(adapter, &major))
		perror("cxl_get_tunneled_ops_supported");
	else
		printf("    %s.tunneled_ops_supported=%ld\n", name, major);
}

void pr_afu_attr(struct cxl_afu_h *afu, int dedicated)
{
	long major;
	enum cxl_prefault_mode prefault_mode;
	char *name = cxl_afu_dev_name(afu);

	if (cxl_get_api_version(afu, &major)) {
		perror("cxl_get_api_version");
		exit(1);
	}
	printf("    %s.api_version=%ld\n", name, major);
	if (cxl_get_api_version_compatible(afu, &major)) {
		perror("cxl_get_api_version_compatible");
		exit(1);
	}
	printf("    %s.api_version_compatible=%ld\n", name, major);
/* cxl_get_cr_* not exported from libcxl yet
	if (cxl_get_cr_class(afu, 0, &major))
		perror("cxl_get_cr_class");
	else
		printf("    %s.cr0.class=0x%lx\n", name, major);
	if (cxl_get_cr_device(afu, 0, &major))
		perror("cxl_get_cr_device");
	else
		printf("    %s.cr0.device=0x%lx\n", name, major);
	if (cxl_get_cr_vendor(afu, 0, &major))
		perror("cxl_get_cr_vendor");
	else
		printf("    %s.cr0.vendor=0x%lx\n", name, major);
*/
	if (cxl_get_irqs_max(afu, &major)) {
		perror("cxl_get_irqs_max");
		exit(1);
	}
	printf("    %s.irqs_max=%ld\n", name, major);
/* cxl_get_dev is not exported from libcxl
	if (dedicated) {
		if (cxl_get_dev(afu, &major, &minor)) {
			perror("cxl_get_dev");
			exit(1);
		}
		printf("    %s.dev.major=%ld\n", name, major);
		printf("    %s.dev.minor=%ld\n", name, minor);
	}
*/
	if (cxl_get_irqs_min(afu, &major)) {
		perror("cxl_get_irqs_min");
		exit(1);
	}
	printf("    %s.irqs_min=%ld\n", name, major);
	if (cxl_get_mmio_size(afu, &major)) {
		perror("cxl_get_mmio_size");
		exit(1);
	}
	printf("    %s.mmio_size=%ld\n", name, major);
	if (cxl_get_mode(afu, &major)) {
		perror("cxl_get_mode");
		exit(1);
	}
	switch(major) {
	case CXL_MODE_DEDICATED:
		printf("    %s.mode=dedicated_process\n", name); break;
	case CXL_MODE_DIRECTED:
		printf("    %s.mode=afu_directed\n", name); break;
	case CXL_MODE_TIME_SLICED:
		printf("    %s.mode=time_sliced\n", name); break;
	}
	if (cxl_get_modes_supported(afu, &major)) {
		perror("cxl_get_modes_supported");
		exit(1);
	}
	if (major & CXL_MODE_DEDICATED)
		printf("    %s.modes_supported=dedicated_process\n", name);
	if (major & CXL_MODE_DIRECTED)
		printf("    %s.modes_supported=afu_directed\n", name);
	if (major & CXL_MODE_TIME_SLICED)
		printf("    %s.modes_supported=time_sliced\n", name);
	if (cxl_get_prefault_mode(afu, &prefault_mode)) {
		perror("cxl_get_prefault_mode");
		exit(1);
	}
	switch (prefault_mode) {
	case CXL_PREFAULT_MODE_NONE:
		printf("    %s.prefault_mode=none\n", name); break;
	case CXL_PREFAULT_MODE_WED:
		printf("    %s.prefault_mode=wed\n", name); break;
	case CXL_PREFAULT_MODE_ALL:
		printf("    %s.prefault_mode=all\n", name); break;
	}
}

void pr_afu_master_attr(struct cxl_afu_h *afu)
{
	long major;
	char *name = cxl_afu_dev_name(afu);
/* cxl_get_cr_* not exported from libcxl yet
	if (cxl_get_cr_class(afu, 0, &major))
		perror("cxl_get_cr_class");
	else
		printf("    %s.cr0.class=0x%lx\n", name, major);
	if (cxl_get_cr_device(afu, 0, &major))
		perror("cxl_get_cr_device");
	else
		printf("    %s.cr0.device=0x%lx\n", name, major);
	if (cxl_get_cr_vendor(afu, 0, &major))
		perror("cxl_get_cr_vendor");
	else
		printf("    %s.cr0.vendor=0x%lx\n", name, major);
*/
/* cxl_get_dev is not exported from libcxl
	if (cxl_get_dev(afu, &major, &minor)) {
		perror("cxl_get_dev");
		exit(1);
	}
	printf("    %s.dev.major=%ld\n", name, major);
	printf("    %s.dev.minor=%ld\n", name, minor);
*/
	if (cxl_get_mmio_size(afu, &major)) {
		perror("cxl_get_mmio_size");
		exit(1);
	}
	printf("    %s.mmio_size=%ld\n", name, major);
	if (cxl_get_pp_mmio_len(afu, &major)) {
		perror("cxl_get_mmio_len");
		exit(1);
	}
	printf("    %s.pp_mmio_len=%ld\n", name, major);
	if (cxl_get_pp_mmio_off(afu, &major)) {
		perror("cxl_get_mmio_off");
		exit(1);
	}
	printf("    %s.pp_mmio_off=%ld\n", name, major);
}

void pr_afu_slave_attr(struct cxl_afu_h *afu)
{
	long major;
	enum cxl_prefault_mode prefault_mode;
	char *name = cxl_afu_dev_name(afu);
/* cxl_get_cr_* not exported from libcxl yet
	if (cxl_get_cr_class(afu, 0, &major))
		perror("cxl_get_cr_class");
	else
		printf("    %s.cr0.class=0x%lx\n", name, major);
	if (cxl_get_cr_device(afu, 0, &major))
		perror("cxl_get_cr_device");
	else
		printf("    %s.cr0.device=0x%lx\n", name, major);
	if (cxl_get_cr_vendor(afu, 0, &major))
		perror("cxl_get_cr_vendor");
	else
		printf("    %s.cr0.vendor=0x%lx\n", name, major);
*/
/* cxl_get_dev is not exported from libcxl
	if (cxl_get_dev(afu, &major, &minor)) {
		perror("cxl_get_dev");
		exit(1);
	}
	printf("    %s.dev.major=%ld\n", name, major);
	printf("    %s.dev.minor=%ld\n", name, minor);
*/
	if (cxl_get_irqs_max(afu, &major)) {
		perror("cxl_get_irqs_max");
		exit(1);
	}
	printf("    %s.irqs_max=%ld\n", name, major);
	if (cxl_get_irqs_min(afu, &major)) {
		perror("cxl_get_irqs_min");
		exit(1);
	}
	printf("    %s.irqs_min=%ld\n", name, major);
	if (cxl_get_mmio_size(afu, &major)) {
		perror("cxl_get_mmio_size");
		exit(1);
	}
	printf("    %s.mmio_size=%ld\n", name, major);
	if (cxl_get_mode(afu, &major)) {
		perror("cxl_get_mode");
		exit(1);
	}
	switch(major) {
	case CXL_MODE_DEDICATED:
		printf("    %s.mode=dedicated_process\n", name); break;
	case CXL_MODE_DIRECTED:
		printf("    %s.mode=afu_directed\n", name); break;
	case CXL_MODE_TIME_SLICED:
		printf("    %s.mode=time_sliced\n", name); break;
	}
	if (cxl_get_modes_supported(afu, &major)) {
		perror("cxl_get_modes_supported");
		exit(1);
	}
	if (major & CXL_MODE_DEDICATED)
		printf("    %s.modes_supported=dedicated_process\n", name);
	if (major & CXL_MODE_DIRECTED)
		printf("    %s.modes_supported=afu_directed\n", name);
	if (major & CXL_MODE_TIME_SLICED)
		printf("    %s.modes_supported=time_sliced\n", name);
	if (cxl_get_prefault_mode(afu, &prefault_mode)) {
		perror("cxl_get_prefault_mode");
		exit(1);
	}
	switch (prefault_mode) {
	case CXL_PREFAULT_MODE_NONE:
		printf("    %s.prefault_mode=none\n", name); break;
	case CXL_PREFAULT_MODE_WED:
		printf("    %s.prefault_mode=wed\n", name); break;
	case CXL_PREFAULT_MODE_ALL:
		printf("    %s.prefault_mode=all\n", name); break;
	}
	if (cxl_get_api_version(afu, &major)) {
		perror("cxl_get_api_version");
		exit(1);
	}
	printf("    %s.api_version=%ld\n", name, major);
}

void set_mode(struct cxl_afu_h *afu, long modes_supported, long new_mode)
{
	long mode;

	if (modes_supported & new_mode) {
		/* New mode is supported. */
		if (cxl_set_mode(afu, new_mode)) {
			perror("cxl_set_mode");
			exit(1);
		}
		if (cxl_get_mode(afu, &mode)) {
			perror("cxl_get_mode");
			exit(1);
		}
		if (mode != new_mode)
			printf("Error: mode set=%ld and get=%ld differ\n",
			       new_mode, mode);
		return;
	}
	/* New mode is not supported. */
	if (!(cxl_set_mode(afu, new_mode) == -1 &&
	      (errno == EBUSY || errno == EINVAL))) {
		printf("Error: cxl_set_mode(%ld) should fail with EBUSY or EINVAL\n",
		       new_mode);
		exit(1);
	}
}

void set_prefault_mode(struct cxl_afu_h *afu, enum cxl_prefault_mode new_mode)
{
	enum cxl_prefault_mode mode;

	if (cxl_set_prefault_mode(afu, new_mode)) {
		perror("cxl_set_prefault_mode");
		exit(1);
	}
	if (cxl_get_prefault_mode(afu, &mode)) {
		perror("cxl_get_prefault_mode");
		exit(1);
	}
	if (mode != new_mode)
		printf("Error: prefault_mode set=%d and get=%d differ\n",
		       new_mode, mode);
}

void wr_afu_attr(struct cxl_afu_h *afu, int attached)
{
	long i, j, mode;
	long irqs_max, irqs_min, modes_supported;
	enum cxl_prefault_mode prefault_mode;

	printf("Setting and resetting irqs_max...\n");
	if (cxl_get_irqs_max(afu, &irqs_max)) {
		perror("cxl_get_irqs_max");
		exit(1);
	}
	if (cxl_get_irqs_min(afu, &irqs_min)) {
		perror("cxl_get_irqs_min");
		exit(1);
	}
	/* Valid irqs_max values. */
	for (i = irqs_min; i <= irqs_max; i++) {
		if (cxl_set_irqs_max(afu, i)) {
			perror("cxl_set_irqs_max");
			exit(1);
		}
		if (cxl_get_irqs_max(afu, &j)) {
			perror("cxl_get_irqs_max");
			exit(1);
		}
		if (i != j) {
			printf("Error: irqs_max set=%ld and get=%ld differ\n",
			       i, j);
			exit(1);
		}
	}
	/* Invalid irqs_max values. */
	if (!(cxl_set_irqs_max(afu, i) == -1 && errno == EINVAL)) {
		printf("Error: cxl_set_irqs_max(%ld) should fail with EINVAL\n", i);
		exit(1);
	}
	for (i = -1; i < irqs_min; i++) {
		if (!(cxl_set_irqs_max(afu, i) == -1 && errno == EINVAL)) {
			printf("Error: cxl_set_irqs_max(%ld) should fail with EINVAL\n", i);
			exit(1);
		}
	}

	/*
	 * Setting the mode should fail:
	 * 1. on AFUs with attached contexts,
	 * 2. on open AFU devices,
	 * 3. when the mode is not supported.
	 */
	printf("Setting and resetting mode...\n");
	if (attached || cxl_afu_opened(afu)) {
		modes_supported = 0;
	} else {
		if (cxl_get_modes_supported(afu, &modes_supported)) {
			perror("cxl_get_modes_supported");
			exit(1);
		}
	}
	if (cxl_get_mode(afu, &mode)) {
		perror("cxl_get_mode");
		exit(1);
	}
	/* Potentially valid mode values (depending on modes_supported). */
	set_mode(afu, modes_supported, CXL_MODE_DEDICATED);
	set_mode(afu, modes_supported, CXL_MODE_DIRECTED);
	set_mode(afu, modes_supported, CXL_MODE_TIME_SLICED);
	set_mode(afu, modes_supported, mode);
	/* Invalid mode values. */
	set_mode(afu, 0, 0);
	set_mode(afu, 0, CXL_MODE_TIME_SLICED*2);

	printf("Setting and resetting prefault_mode...\n");
	if (cxl_get_prefault_mode(afu, &prefault_mode)) {
		perror("cxl_get_prefault_mode");
		exit(1);
	}
	/* Valid prefault_mode values. */
	set_prefault_mode(afu, CXL_PREFAULT_MODE_NONE);
	set_prefault_mode(afu, CXL_PREFAULT_MODE_WED);
	set_prefault_mode(afu, CXL_PREFAULT_MODE_ALL);
	set_prefault_mode(afu, prefault_mode);
	/* Invalid prefault_mode values. */
	if (!(cxl_set_prefault_mode(afu, -1) == -1 && errno == EINVAL)) {
		printf("Error: cxl_set_prefault_mode(1) should fail with EINVAL\n");
		exit(1);
	}
	if (!(cxl_set_prefault_mode(afu, 999) == -1 && errno == EINVAL)) {
		printf("Error: cxl_set_prefault_mode(999) should fail with EINVAL\n");
		exit(1);
	}
	return;
}

int main(int argc, char *argv[])
{
	struct cxl_adapter_h *adapter_h;
	struct cxl_afu_h *afu_d;
	struct cxl_afu_h *afu_h;
	struct cxl_afu_h *afu_m;
	struct cxl_afu_h *afu_s;
	int afu_fd;
	char *name;
	long mode;

	/* Check if we are running in radix mode */
	if (set_isRadix())
		exit(1);

	printf("Enumerating CXL cards and AFUs...\n");
	cxl_for_each_adapter(adapter_h) {
		name = cxl_adapter_dev_name(adapter_h);
		printf("  Enumerated CXL adapter %s\n", name);
		pr_adapter_attr(adapter_h);
		cxl_for_each_adapter_afu(adapter_h, afu_h) {
			printf("    Enumerated CXL AFU %s\n", cxl_afu_dev_name(afu_h));
		}
	}
	if (errno)	/* Do not return now: cxl_for_each_afu will. */
		perror("cxl_for_each_adapter");

	printf("\nEnumerating all AFUs...\n");
	cxl_for_each_afu(afu_h) {
		name = cxl_afu_dev_name(afu_h);
		printf("  Enumerated CXL AFU %s\n", name);
		pr_afu_attr(afu_h, 0);
		pr_cxl_sysfs(afu_h);
		wr_afu_attr(afu_h, 0);
		cxl_get_mode(afu_h, &mode);
		if (mode == CXL_MODE_DEDICATED) {
			printf("\nOpening AFU dedicated by AFU handle...\n");
			if ((afu_d = cxl_afu_open_h(afu_h, CXL_VIEW_DEDICATED)) == NULL) {
				perror("cxl_afu_open_h CXL_VIEW_DEDICATED");
				return 1;
			}
			pr_afu_attr(afu_d, 1);
			pr_cxl_sysfs(afu_d);
			if (attach_afu(afu_d) == -1) {
				perror("Unable to attach /dev/cxl/afu0.0d");
				return 1;
			}
			wr_afu_attr(afu_d, 1);
			cxl_afu_free(afu_d);
			continue;
		}
		/* CXL_MODE_DIRECTED */
		printf("\nOpening AFU master by AFU handle...\n");
		if ((afu_m = cxl_afu_open_h(afu_h, CXL_VIEW_MASTER)) == NULL) {
			perror("cxl_afu_open_h CXL_VIEW_MASTER");
			return 1;
		}
		pr_afu_master_attr(afu_m);
		pr_cxl_sysfs(afu_m);
		if (attach_afu(afu_m) == -1) {
			perror("Unable to attach /dev/cxl/afu0.0m");
			return 1;
		}
		wr_afu_attr(afu_h, 1);

		printf("\nOpening AFU slave by AFU handle...\n");
		if ((afu_s = cxl_afu_open_h(afu_m, CXL_VIEW_SLAVE)) == NULL) {
			perror("cxl_afu_open_h CXL_VIEW_SLAVE");
			return 1;
		}
		cxl_afu_free(afu_m);
		pr_afu_slave_attr(afu_s);
		pr_cxl_sysfs(afu_s);
		if (attach_afu(afu_s) == -1) {
			perror("Unable to attach /dev/cxl/afu0.0s");
			return 1;
		}
		wr_afu_attr(afu_h, 1);
		cxl_afu_free(afu_s);
	}
	if (errno) {
		perror("cxl_for_each_afu");
		return 1;
	}

	afu_h = cxl_afu_open_dev("/dev/cxl/afu0.0");
	cxl_get_mode(afu_h, &mode);
	if (mode == CXL_MODE_DEDICATED)
		return 0;
	/* CXL_MODE_DIRECTED */
	printf("\nOpening AFU master by known path...\n");
	if ((afu_m = cxl_afu_open_dev("/dev/cxl/afu0.0m")) == NULL) {
		perror("Unable to open cxl device /dev/cxl/afu0.0m");
		return 1;
	}
	pr_afu_master_attr(afu_m);
	pr_cxl_sysfs(afu_m);
	wr_afu_attr(afu_m, 0);
	cxl_afu_free(afu_m);

	printf("\nOpening AFU master by known path via symlink...\n");
	if (symlink("/dev/cxl/afu0.0m", "/tmp/master") == -1) {
		perror("Unable to create symlink /tmp/master -> /dev/cxl/afu0.0m");
		return 1;
	}
	if ((afu_m = cxl_afu_open_dev("/tmp/master")) == NULL) {
		perror("Unable to open cxl device /tmp/master -> /dev/cxl/afu0.0m");
		return 1;
	}
	if (unlink("/tmp/master") == -1) {
		perror("Unable to remove symlink /tmp/master");
		return 1;
	}
	pr_afu_master_attr(afu_m);
	pr_cxl_sysfs(afu_m);
	cxl_afu_free(afu_m);

	printf("\nOpening AFU slave externally...\n");
	if ((afu_fd = open("/dev/cxl/afu0.0s", O_RDWR | O_CLOEXEC)) == -1) {
		perror("Unable to open cxl device /dev/cxl/afu0.0s");
		return 1;
	}
	if ((afu_s = cxl_afu_fd_to_h(afu_fd)) == NULL) {
		perror("cxl_afu_fd_to_h");
		return 1;
	}
	pr_afu_slave_attr(afu_s);
	pr_cxl_sysfs(afu_s);
	wr_afu_attr(afu_s, 0);
	cxl_afu_free(afu_s);

	printf("\nOpening AFU slave externally via symlink...\n");
	if (symlink("/dev/cxl/afu0.0s", "/tmp/slave") == -1) {
		perror("Unable to create symlink /tmp/slave -> /dev/cxl/afu0.0s");
		return 1;
	}
	if ((afu_fd = open("/tmp/slave", O_RDWR | O_CLOEXEC)) == -1) {
		perror("Unable to open cxl device /tmp/slave -> /dev/cxl/afu0.0s");
		return 1;
	}
	if (unlink("/tmp/slave") == -1) {
		perror("Unable to remove symlink /tmp/slave");
		return 1;
	}
	if ((afu_s = cxl_afu_fd_to_h(afu_fd)) == NULL) {
		perror("cxl_afu_fd_to_h");
		return 1;
	}
	pr_afu_slave_attr(afu_s);
	pr_cxl_sysfs(afu_s);
	cxl_afu_free(afu_s);
	return 0;
}
