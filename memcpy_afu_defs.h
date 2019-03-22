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

#ifndef _CXL_MEMCPY_H
#define _CXL_MEMCPY_H

#include <linux/types.h>

/* Hardware definitions for the memcpy AFU.  All big endian */
struct memcpy_work_element {
	volatile __u8 cmd;	/* valid, wrap, cmd */
	volatile __u8 status;
	__be16 length;		/* also irq src */
	__u8 cmd_extra;
	__u8 reserved1;
	__u16 reserved2;
	__be64 atomic_op1;
	__be64 src;		/* also atomic_op2 */
	__be64 dst;
};

#define MEMCPY_WE_CMD(valid, cmd) \
	((((valid) & 0x1) << 7) |	\
	 (((cmd) & 0x3f) << 0))
#define MEMCPY_WE_CMD_COPY	0
#define MEMCPY_WE_CMD_IRQ	1
#define MEMCPY_WE_CMD_INCR	4
#define MEMCPY_WE_CMD_ATOMIC	5
#define MEMCPY_WE_STAT_COMPLETE		0x80
#define MEMCPY_WE_STAT_TRANS_FAULT	0x40
#define MEMCPY_WE_STAT_AERROR		0x20
#define MEMCPY_WE_STAT_DERROR		0x10
#define MEMCPY_WE_STAT_PSL_FAULT	0x08
#define MEMCPY_WE_STAT_INV_SRC		0x04
#define MEMCPY_WE_STAT_PROC_TERM	0x02
#define MEMCPY_WE_STAT_UNDEF_CMD	0x01

#define MEMCPY_WE_CMD_VALID (0x1 << 7)
#define MEMCPY_WE_CMD_WRAP (0x1 << 6)

#define MEMCPY_WED(queue, depth)  \
	((((uintptr_t)queue) & 0xfffffffffffff000ULL) | \
	 (((__u64)depth) & 0xfffULL))

/* AFU descriptor */
#define MEMCPY_AFUD_NUM_OF_PROCESSES	0x01ff

/* AFU PSA registers */
#define MEMCPY_AFU_PSA_REGS_SIZE 16*1024
#define MEMCPY_AFU_PSA_REG_CFG		0
#define MEMCPY_AFU_PSA_REG_ERR		2
#define MEMCPY_AFU_PSA_REG_ERR_INFO	3
#define MEMCPY_AFU_PSA_REG_TRACE_CTL	4

/* AFU atomic commands */
# define MEMCPY_WE_CMD_EXTRA_CAS_NOT_EQUAL_8	0x10
# define MEMCPY_WE_CMD_EXTRA_CAS_EQUAL_8	0x11

/* AFU Configuration Register bits */
#define MEMCPY_AFU_PSA_REG_CFG_Stop_on_Inv_Cmd	0x2000000000000000ULL

/* Per-process application registers */
#define MEMCPY_PS_REG_WED	(0 << 3)
#define MEMCPY_PS_REG_PH	(1 << 3)
#define MEMCPY_PS_REG_STATUS	(2 << 3)
#define MEMCPY_PS_REG_PCTRL	(3 << 3)
#define MEMCPY_PS_REG_TB	(8 << 3)

/* Process Status Register bits */
#define MEMCPY_PS_REG_STATUS_Stopped	0x0800000000000000ULL

/* Process Control Register bits */
#define MEMCPY_PS_REG_PCTRL_Restart	0x8000000000000000ULL

#endif
