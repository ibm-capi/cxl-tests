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

#ifndef _UAPI_MISC_CXL_MEMCPY_H
#define _UAPI_MISC_CXL_MEMCPY_H

#include <linux/ioctl.h>
#include <misc/cxl.h>

/* ioctl numbers */
#define CXL_MEMCPY_MAGIC 0xC9
#define CXL_MEMCPY_IOCTL_GET_FD		_IOW(CXL_MEMCPY_MAGIC, 0x00, int)

#define CXL_MEMCPY_IOCTL_GET_FD_MASTER	0x0000000000000001UL
#define CXL_MEMCPY_IOCTL_GET_FD_ALL	0x0000000000000001UL

struct cxl_memcpy_ioctl_get_fd {
	struct cxl_ioctl_start_work work;
	__u64 master;
};

#endif
