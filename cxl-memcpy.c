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

#include <linux/pci.h>
#include <linux/module.h>
#include <linux/module.h>
#include <linux/semaphore.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/file.h>
#include <linux/delay.h>

#include <asm/atomic.h>
#include <asm/uaccess.h>
#include <asm/barrier.h>

#include "cxl-memcpy.h"
#include "memcpy_afu_defs.h"

static const struct pci_device_id cxl_memcpy_pci_tbl[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_IBM, 0x4350), },
	{ }
};
MODULE_DEVICE_TABLE(pci, cxl_memcpy_pci_tbl);

uint cpu_memcopy;
module_param_named(cpu_memcopy, cpu_memcopy, uint, 0600);
MODULE_PARM_DESC(cpu_memcopy, "Use CPU to perform memcpy");

#define DEVICENAME "cxlmemcpy"
#define CLASSNAME "cxltest"
#define MINOR_MAX 1
static int major_number;
static atomic_t minor_number;
static struct semaphore sem;
static struct cdev *cdev;
static dev_t dev_num;
static struct pci_dev *memcpy_afu_dev;
static struct class *cxltest_class;

/* copy buffers.  This afu requires cachline alignment (ie 128 bytes) */
#define BUFFER_SIZE 1024
static char write_buf[BUFFER_SIZE] __aligned(128);
static char read_buf[BUFFER_SIZE] __aligned(128);

#define MEMCPY_QUEUE_ENTRIES 4095*2
#define MEMCPY_QUEUE_SIZE (MEMCPY_QUEUE_ENTRIES * sizeof(struct memcpy_work_element))
static struct memcpy_work_element cxl_memcpy_queue[MEMCPY_QUEUE_ENTRIES] __aligned(PAGE_SIZE);

struct cxl_memcpy_info {
	bool afu_irq_done;
};

#define VPD_SIZE 4096 * 8

static void cxl_memcpy_vpd_info(struct pci_dev *dev)
{
	void *buf;
	ssize_t len;
	int i, max;
	unsigned long *ptr;

	buf = kmalloc(VPD_SIZE, GFP_KERNEL);
	if (buf == NULL) {
		printk("out of memory to read VPD\n");
		return;
	}

	len = cxl_read_adapter_vpd(dev, buf, VPD_SIZE);
	printk("VPD size=%#lx\n", len);

	max = len / sizeof(*ptr);
	if (max > 0x10)
		max = 0x10;
	ptr = (unsigned long *) buf;
	for (i = 0; i < max; i++, ptr++) {
		printk("VPD\t%x:\t%016lx\n", i, *ptr);
	}

	kfree(buf);
}

static irqreturn_t cxl_memcpy_irq_afu(int irq, void *data)
{
	struct cxl_memcpy_info *info = data;

	smp_mb();
	info->afu_irq_done = true;

	return IRQ_HANDLED;
}

static irqreturn_t cxl_memcpy_copy_error(int irq, void *data)
{
	printk("%s IRQ %i Copy error!\n", __func__, irq);
	return IRQ_HANDLED;
}

static irqreturn_t cxl_memcpy_afu_error(int irq, void *data)
{
	printk("%s IRQ %i AFU error!\n", __func__, irq);
	return IRQ_HANDLED;
}

/*
 * Setup the memcpy AFU queue.  This is memcpy AFU specific.  It needs
 * two entries, one to do the actual copy and a second the send the
 * interrupt.  The last entry is blank so the afu doesn't run off the
 * end of the queue.
 */
void cxl_memcpy_setup_queue(void)
{
	memset(cxl_memcpy_queue, 0, MEMCPY_QUEUE_SIZE);
	/* first entry: Copy */
	cxl_memcpy_queue[0].cmd = MEMCPY_WE_CMD(0, MEMCPY_WE_CMD_COPY);
	cxl_memcpy_queue[0].status = 0;
	cxl_memcpy_queue[0].length = cpu_to_be16(BUFFER_SIZE);
	cxl_memcpy_queue[0].src = cpu_to_be64((u64)write_buf);
	cxl_memcpy_queue[0].dst = cpu_to_be64((u64)read_buf);
	/* second entry: generate IRQ */
	cxl_memcpy_queue[1].cmd = MEMCPY_WE_CMD(1, MEMCPY_WE_CMD_IRQ);
	cxl_memcpy_queue[1].status = 0;
	cxl_memcpy_queue[1].length = cpu_to_be16(1);
	cxl_memcpy_queue[1].src = 0;
	cxl_memcpy_queue[1].dst = 0;
	/* third entry: left empty */

	/* Make sure this hits memory before we start the AFU */
	mb();
}

/*
 * Set the valid bit on the first entry in the queue, so that the
 * AFU starts its processing.
 */
void cxl_memcpy_activate_queue(void)
{
	cxl_memcpy_queue[0].cmd |= MEMCPY_WE_CMD_VALID;
}

/*
 * Start the afu context.  This is calling into the generic CXL driver code
 * (except for the contents of the WED).
 */
int cxl_memcpy_start_context(struct cxl_context *ctx)
{
	u64 wed;

	wed = MEMCPY_WED(cxl_memcpy_queue,
			 MEMCPY_QUEUE_SIZE/SMP_CACHE_BYTES);
	return cxl_start_context(ctx, wed, NULL);
}

/* use memcpy afu to copy write_buf[] to read_buf[] */
static int memcpy_afu(struct pci_dev *dev)
{
	struct cxl_context *ctx;
	struct cxl_memcpy_info *info;
	void __iomem *psa;
	int rc = 0;

	info = kzalloc(sizeof(struct cxl_memcpy_info), GFP_KERNEL);

	/* Get default context.  Can do this or create a new one */
	ctx = cxl_get_context(memcpy_afu_dev);
	if (!ctx)
		return -ENOMEM;

	rc = cxl_afu_reset(ctx);
	if (rc)
		goto err1;

	/* Allocate AFU generated interrupt handler */
	rc = cxl_allocate_afu_irqs(ctx, 4);
	if (rc)
		goto err1;

	/* Register AFU interrupt 1. */
	rc = cxl_map_afu_irq(ctx, 1, cxl_memcpy_irq_afu, info, "afu1");
	if (!rc)
		goto err2;
	/* Register AFU interrupt 2 for errors. */
	rc = cxl_map_afu_irq(ctx, 2, cxl_memcpy_copy_error, ctx, "err1");
	if (!rc)
		goto err3;
	/* Register AFU interrupt 3 for errors. */
	rc = cxl_map_afu_irq(ctx, 3, cxl_memcpy_afu_error, ctx, "err2");
	if (!rc)
		goto err4;
	/* Register AFU interrupt 4 for errors. */
	rc = cxl_map_afu_irq(ctx, 4, cxl_memcpy_afu_error, ctx, "err3");
	if (!rc)
		goto err5;

	/* Register for PSL errors.  TODO: implement this */
	//cxl_register_error_irq(dev, flags??, callback function, private data);

	cxl_set_master(ctx);

	/* Setup memcpy AFU work queue */
	cxl_memcpy_setup_queue();

	/* Mark interrupt as incomplete */
	info->afu_irq_done = false;
	smp_mb();

	/* Start Context on AFU */
	rc = cxl_memcpy_start_context(ctx);
	if (rc) {
		dev_err(&dev->dev, "Can't start context");
		goto err6;
	}

	/* Map AFU MMIO/Problem space area */
	psa = cxl_psa_map(ctx);
	if (!psa)
		goto err7;

	/* Write configuration info to the AFU PSA space */
	out_be64(psa + 0, 0x8000000000000000ULL);

	/* Flip valid bit on first job on the queue */
	cxl_memcpy_activate_queue();

	/*
	 * Wait for interrupt to complete.  We'd never do it this way in
	 * practice, this is just a demonstration that the AFU IRQ works.
	 */
	rc = 0;
	while(!info->afu_irq_done && rc < 1000) {
		msleep(1);
		rc++;
	}

	if (!info->afu_irq_done)
		printk("Didn't receive end of job interrupt\n");

	rc = 0;

	/* Copy is complete!  Lets tear things down again */
	cxl_psa_unmap(psa);
err7:
	cxl_stop_context(ctx);
err6:
	cxl_unmap_afu_irq(ctx, 4, ctx);
err5:
	cxl_unmap_afu_irq(ctx, 3, ctx);
err4:
	cxl_unmap_afu_irq(ctx, 2, ctx);
err3:
	cxl_unmap_afu_irq(ctx, 1, info);
err2:
	cxl_free_afu_irqs(ctx);
err1:
	kfree(info);
	return rc;
}

static ssize_t device_read(struct file *fp, char __user *buff, size_t length,
			   loff_t *ppos)
{
	int max_bytes;
	int bytes_to_read;
	int bytes_read;
	int rc;

	if (cpu_memcopy)
		memcpy(read_buf, write_buf, BUFFER_SIZE);
	else {
		rc = memcpy_afu(memcpy_afu_dev);
		if (rc) {
			return rc;
		}
	}

	max_bytes = BUFFER_SIZE - *ppos;
	if(max_bytes > length)
		bytes_to_read = length;
	else
		bytes_to_read = max_bytes;

	bytes_read = bytes_to_read - copy_to_user(buff, read_buf + *ppos,
						  bytes_to_read);
	*ppos += bytes_read;
	return bytes_read;
}

static ssize_t device_write(struct file *fp, const char __user  *buff,
			    size_t length, loff_t *ppos)
{
	int max_bytes;
	int bytes_to_write;
	int bytes_writen;

	max_bytes = BUFFER_SIZE - *ppos;
	if(max_bytes > length)
		bytes_to_write = length;
	else
		bytes_to_write = max_bytes;
	bytes_writen = bytes_to_write - copy_from_user(write_buf + *ppos,
						       buff, bytes_to_write);
	*ppos += bytes_writen;
	return bytes_writen;
}

static int device_open(struct inode *inode, struct file *file)
{
	if (down_interruptible(&sem) != 0) {
		return -1;
	}
	file->private_data = memcpy_afu_dev;
	return 0;
}

static int device_close(struct inode *inode, struct file *file) {
	up(&sem);
	return 0;
}

static int device_afu_release(struct inode *inode, struct file *file)
{
	/* just call the AFU one for now */
	return cxl_fd_release(inode, file);
}

struct file_operations cxl_memcpy_fops = {
	.owner		= THIS_MODULE,
	.release        = device_afu_release,
};

static long device_ioctl_get_fd(struct pci_dev *dev,
				struct cxl_memcpy_ioctl_get_fd __user *arg)
{
	struct cxl_memcpy_ioctl_get_fd work;
	struct cxl_context *ctx = NULL;
	struct file *file;
	int rc, fd;

	/* Copy the user info */
	if (copy_from_user(&work, arg, sizeof(struct cxl_memcpy_ioctl_get_fd)))
		return -EFAULT;

	/* Init the context */
	ctx = cxl_dev_context_init(dev);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	/* Does the user want a master context? */
	if (work.master & CXL_MEMCPY_IOCTL_GET_FD_MASTER)
		cxl_set_master(ctx);

	/* Create and attach a new file descriptor */
	file = cxl_get_fd(ctx, &cxl_memcpy_fops, &fd);

	rc = cxl_start_work(ctx, &work.work);
	if (rc) {
		fput(file);
		put_unused_fd(fd);
		return -ENODEV;
	}
	/* No error paths after installing the fd */
	fd_install(fd, file);
	return fd;
}

static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct pci_dev *dev = file->private_data;

	pr_devel("device_ioctl\n");
	switch (cmd) {
	case CXL_MEMCPY_IOCTL_GET_FD:
		return device_ioctl_get_fd(dev,
				(struct cxl_memcpy_ioctl_get_fd __user *)arg);
	}
	return -EINVAL;
}

struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = device_open,
	.write = device_write,
	.read = device_read,
	.llseek = generic_file_llseek,
	.release = device_close,
	.unlocked_ioctl = device_ioctl,
};

static int cxl_memcpy_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	struct device *sysdev;
	struct cxl_afu *afu;
	struct page *dummypage;
	dma_addr_t map;
	int rc;
	int minor;

	minor = atomic_inc_return(&minor_number);
	if (minor >= MINOR_MAX) {
		atomic_dec(&minor_number);
		return -ENOSPC; /* we only support 1 currently */
	}

	cdev = cdev_alloc();
	cdev->ops = &fops;
	cdev->owner = THIS_MODULE;

	rc = cdev_add(cdev,dev_num,MINOR_MAX);
	if (rc < 0) {
		printk(KERN_ALERT " device adding to the kernel failed\n");
		return rc;
	}

	sysdev = device_create(cxltest_class, NULL, dev_num, NULL, DEVICENAME);
	if (IS_ERR(sysdev)) {
		rc = PTR_ERR(sysdev);
		pr_err("Unable to create "DEVICENAME" device in sysfs: %i\n",
			rc);
		goto err1;
	}

	rc = pci_enable_device(dev);
	if (rc)
		goto err2;

	afu = cxl_pci_to_afu(dev);
	sema_init(&sem, 1);
	memcpy_afu_dev = dev;

	cxl_memcpy_vpd_info(dev);

	/* Try out the dma ops */
	dummypage = alloc_page(GFP_KERNEL);
	map = dma_map_single(&dev->dev, dummypage, PAGE_SIZE, DMA_BIDIRECTIONAL);
	printk("map:%016lx dummypage:%p phys:%016lx\n", (unsigned long int)map, dummypage,
	       virt_to_phys(dummypage));

	return 0;
err2:
	device_destroy(cxltest_class, dev_num);
err1:
	cdev_del(cdev);
	atomic_dec(&minor_number);
	return rc;
}

static void cxl_memcpy_remove(struct pci_dev *dev)
{
	pci_disable_device(dev);
	device_destroy(cxltest_class, dev_num);
	cdev_del(cdev);
	atomic_dec(&minor_number);
	printk("%s\n", __func__);
}

static pci_ers_result_t pci_error_detected(struct pci_dev *pdev,
					pci_channel_state_t state)
{
	printk("cxl-memcpy: %s called, state=%d\n", __func__, state);
	switch (state) {
	case pci_channel_io_frozen:
		return PCI_ERS_RESULT_NEED_RESET;
	case pci_channel_io_perm_failure:
		return PCI_ERS_RESULT_DISCONNECT;
	default:
		break;
	}
	return PCI_ERS_RESULT_NEED_RESET;
}

static pci_ers_result_t pci_slot_reset(struct pci_dev *pdev)
{
	printk("cxl-memcpy: %s called\n", __func__);
	return PCI_ERS_RESULT_RECOVERED;
}

static void pci_resume(struct pci_dev *pdev)
{
	printk("cxl-memcpy: %s called\n", __func__);
}

static const struct pci_error_handlers err_handler = {
	.error_detected = pci_error_detected,
	.slot_reset = pci_slot_reset,
	.resume = pci_resume,
};

struct pci_driver cxl_memcpy_pci_driver = {
	.name = "cxl-memcpy",
	.id_table = cxl_memcpy_pci_tbl,
	.probe = cxl_memcpy_probe,
	.remove = cxl_memcpy_remove,
	.err_handler = &err_handler,
};

static int __init init_cxl_memcpy(void)
{
	int rc = 0;

	rc = alloc_chrdev_region(&dev_num,0,MINOR_MAX,DEVICENAME);
	if (rc < 0) {
		pr_err("failed to allocate major number\n");
		return rc;
	}
	major_number = MAJOR(dev_num);
	atomic_set(&minor_number, -1);

	cxltest_class = class_create(THIS_MODULE, CLASSNAME);
	if (IS_ERR(cxltest_class)) {
		rc = PTR_ERR(cxltest_class);
		pr_err("Unable to create "CLASSNAME" class: %i\n", rc);
		goto err;
	}

	rc = pci_register_driver(&cxl_memcpy_pci_driver);
	if (rc)
		goto err1;

	return 0;
err1:
	class_destroy(cxltest_class);
err:
	unregister_chrdev_region(dev_num, MINOR_MAX);
	return rc;

}
static void exit_cxl_memcpy(void)
{
	pci_unregister_driver(&cxl_memcpy_pci_driver);
	class_destroy(cxltest_class);
	unregister_chrdev_region(dev_num, MINOR_MAX);
}

module_init(init_cxl_memcpy);
module_exit(exit_cxl_memcpy);


MODULE_DESCRIPTION("IBM CXL memcpy AFU");
MODULE_AUTHOR("Michael Neuling <mikey@neuling.org>>");
MODULE_LICENSE("GPL");
