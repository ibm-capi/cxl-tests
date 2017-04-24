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

#ifndef _MEMCPY_AFU_H_
#define _MEMCPY_AFU_H_

#include <linux/types.h>
#include <assert.h>
#include "memcpy_afu_defs.h"

struct memcpy_weq {
	struct memcpy_work_element *queue;
	struct memcpy_work_element *next;
	struct memcpy_work_element *last;
	int wrap;
	int count;
};

#define mb()   __asm__ __volatile__ ("sync" : : : "memory")

static inline int memcpy_queue_length(size_t queue_size)
{
	return queue_size/sizeof(struct memcpy_work_element);
}

void memcpy_init_weq(struct memcpy_weq *weq, size_t queue_size);
struct memcpy_work_element *memcpy_add_we(struct memcpy_weq *weq, struct memcpy_work_element we);

#endif /* _MEMCPY_AFU_H_ */
