// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/mmc/host.h>
#include <linux/mmc/sdio_func.h>
#include <linux/mmc/card.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/sdio_ids.h>
#include <linux/cdev.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_net.h>
#include <net/mac80211.h>

#include "cw1200.h"
#include "hwbus.h"
#include "hwio.h"

#include <linux/types.h>
#include <linux/ioctl.h>

struct besdbg_data {
	__u32 reg;
	__u32 len;
	__u64 data;
};

#define BESDBG_MAGIC 0xEE

#define BESDBG_IOCTL_RESET	_IO(BESDBG_MAGIC, 0x10)
#define BESDBG_IOCTL_REG_READ	_IOR(BESDBG_MAGIC, 0x11, struct besdbg_data)
#define BESDBG_IOCTL_REG_WRITE	_IOW(BESDBG_MAGIC, 0x12, struct besdbg_data)
#define BESDBG_IOCTL_MEM_READ	_IOR(BESDBG_MAGIC, 0x13, struct besdbg_data)
#define BESDBG_IOCTL_MEM_WRITE	_IOW(BESDBG_MAGIC, 0x14, struct besdbg_data)

#define SDIO_BLOCK_SIZE (512)

static struct class *besdbg_class;

struct besdbg_priv {
	struct sdio_func	*func;
	struct gpio_desc	*wakeup_device_gpio;
	struct cdev		cdev;
	dev_t			major;
};

/* bes sdio slave regs can only be accessed by command52
 * if a WORD or DWORD reg wants to be accessed,
 * please combine the results of multiple command52
 */
static int bes2600_sdio_reg_read(struct besdbg_priv *self, u32 reg,
				 void *dst, int count)
{
	int ret = 0;
	if (count <= 0 || !dst)
		return -EINVAL;

	while (count && !ret) {
		*(u8 *)dst = sdio_readb(self->func, reg, &ret);

		dst++;
		reg++;
		count--;
	}

	return ret;
}

static int bes2600_sdio_reg_write(struct besdbg_priv *self, u32 reg,
				  const void *src, int count)
{
	int ret = 0;
	if (count <= 0 || !src)
		return -EINVAL;

	while (count && !ret) {
		sdio_writeb(self->func, *(u8 *)src, reg, &ret);

		src++;
		reg++;
		count--;
	}

	return ret;
}

#if 0
static int bes2600_sdio_mem_helper(struct besdbg_priv *self, u8 *data, int count, int write)
{
	int off = 0;
	int ret;

	while (off < count) {
		int block = min(count - off, );

		if (write)
			ret = sdio_memcpy_toio(func, block, data + off, block);
		else
			ret = sdio_memcpy_fromio(func, data + off, block, block);
		if (ret)
			return ret;

		off += size;
	}
	
	return 0;
}
#endif

#if 0
static unsigned int besdbg_poll(struct file *fp, poll_table *wait)
{
	struct besdbg_priv* besdbg = fp->private_data;

	poll_wait(fp, &besdbg->wait, wait);

	if (!kfifo_is_empty(&besdbg->kfifo))
		return EPOLLIN | EPOLLRDNORM;

	return 0;
}
#endif

static int besdbg_open(struct inode *ip, struct file *fp)
{
	struct besdbg_priv* besdbg = container_of(ip->i_cdev,
						  struct besdbg_priv, cdev);

	fp->private_data = besdbg;

	nonseekable_open(ip, fp);
	return 0;
}

static int besdbg_release(struct inode *ip, struct file *fp)
{
//	struct besdbg_priv* besdbg = fp->private_data;

	return 0;
}

long besdbg_ioctl(struct file *fp, unsigned int cmd, unsigned long arg)
{
	struct besdbg_priv* self = fp->private_data;
	struct device *dev = &self->func->dev;
	void __user *argp = (void __user *)arg;
	long ret;

	switch (cmd) {
	case BESDBG_IOCTL_RESET:
		/* Disable the card */
		gpiod_direction_output(self->wakeup_device_gpio, 0);

		sdio_claim_host(self->func);

		ret = mmc_hw_reset(self->func->card);
		if (ret)
			dev_warn(dev, "unable to reset sdio: %ld\n", ret);

		gpiod_direction_output(self->wakeup_device_gpio, 1);
		msleep(10);

		ret = sdio_enable_func(self->func);
		if (ret) {
			sdio_release_host(self->func);
			return ret;
		}

		sdio_release_host(self->func);

		return 0;

	case BESDBG_IOCTL_REG_READ: {
		struct besdbg_data r;
	
		if (copy_from_user(&r, argp, sizeof(r)))
			return -EFAULT;

		if (r.len > 32 || r.len == 0 || !r.data)
			return -EINVAL;

		u8 *data = kmalloc(r.len, GFP_KERNEL);
		if (!data)
			return -ENOMEM;

		sdio_claim_host(self->func);
		ret = bes2600_sdio_reg_read(self, r.reg, data, r.len);
		sdio_release_host(self->func);
		
		if (ret) {
			dev_err(dev, "read failed\n");
			kfree(data);
			return ret;
		}

		if (copy_to_user((void __user *)r.data, data, r.len)) {
			kfree(data);
			return -EFAULT;
		}

		kfree(data);
		return 0;
	}

	case BESDBG_IOCTL_REG_WRITE: {
		struct besdbg_data r;
	
		if (copy_from_user(&r, argp, sizeof(r)))
			return -EFAULT;

		if (r.len > 64 * 1024 || r.len == 0 || !r.data)
			return -EINVAL;

		u8 *data = kmalloc(r.len, GFP_KERNEL);
		if (!data)
			return -ENOMEM;

		if (copy_from_user(data, (void __user *)r.data, r.len))
			return -EFAULT;

		sdio_claim_host(self->func);
		ret = bes2600_sdio_reg_write(self, r.reg, data, r.len);
		sdio_release_host(self->func);
		
		kfree(data);

		if (ret) {
			dev_err(dev, "read failed\n");
			return ret;
		}

		return 0;
	}

	case BESDBG_IOCTL_MEM_READ: {
		struct besdbg_data r;
	
		if (copy_from_user(&r, argp, sizeof(r)))
			return -EFAULT;

		if (r.len > 1024 * 64 || r.len == 0 || !r.data)
			return -EINVAL;

		u8 *data = kmalloc(r.len, GFP_KERNEL);
		if (!data)
			return -ENOMEM;

		sdio_claim_host(self->func);
		//ret = bes2600_sdio_mem_helper(self, data, r.len, 0);
		ret = sdio_memcpy_fromio(self->func, data, r.reg, r.len);
		sdio_release_host(self->func);
		
		if (ret) {
			dev_err(dev, "read failed\n");
			kfree(data);
			return ret;
		}

		if (copy_to_user((void __user *)r.data, data, r.len)) {
			kfree(data);
			return -EFAULT;
		}

		kfree(data);
		return 0;
	}

	case BESDBG_IOCTL_MEM_WRITE: {
		struct besdbg_data r;
	
		if (copy_from_user(&r, argp, sizeof(r)))
			return -EFAULT;

		if (r.len > 64 * 1024 || r.len == 0 || !r.data)
			return -EINVAL;

		u8 *data = kmalloc(r.len, GFP_KERNEL);
		if (!data)
			return -ENOMEM;

		if (copy_from_user(data, (void __user *)r.data, r.len))
			return -EFAULT;

		sdio_claim_host(self->func);
		//ret = bes2600_sdio_mem_helper(self, data, r.len, 1);
		ret = sdio_memcpy_toio(self->func, r.reg, data, r.len);
		sdio_release_host(self->func);
		
		kfree(data);

		if (ret) {
			dev_err(dev, "read failed\n");
			return ret;
		}

		return 0;
	}
	}

	return -EINVAL;
}

static const struct file_operations besdbg_fops = {
	.owner		= THIS_MODULE,
	.open		= besdbg_open,
	.release	= besdbg_release,
	.unlocked_ioctl	= besdbg_ioctl,
	.llseek		= noop_llseek,
	//.read		= besdbg_read,
	//.poll		= besdbg_poll,
};

static int besdbg_probe(struct sdio_func *func, const struct sdio_device_id *id)
{
	struct device *dev = &func->dev;
	struct besdbg_priv *self;
	int ret;

	if (func->num != 0x01)
		return -ENODEV;

	dev_info(dev, "probe start\n");

	if (!of_device_is_compatible(dev->of_node, "bestechnic,bes2600")) {
		dev_err(dev, "OF node for function 1 is missing\n");
		return -ENODEV;
	}

	self = devm_kzalloc(dev, sizeof(*self), GFP_KERNEL);
	if (!self)
		return -ENOMEM;

	self->func = func;

	func->card->quirks |= MMC_QUIRK_LENIENT_FN0;
	func->card->quirks |= MMC_QUIRK_BROKEN_BYTE_MODE_512;

	self->wakeup_device_gpio = devm_gpiod_get(dev, "device-wakeup", GPIOD_OUT_LOW);
	if (IS_ERR(self->wakeup_device_gpio))
		return dev_err_probe(dev, PTR_ERR(self->wakeup_device_gpio),
				     "can't get device-wakeup gpio\n");

	if (self->wakeup_device_gpio) {
		gpiod_direction_output(self->wakeup_device_gpio, 1);
		msleep(10);
	}

	ret = alloc_chrdev_region(&self->major, 0, 1, "besdbg");
	if (ret) {
		dev_err(dev, "can't allocate chrdev region");
		goto err_sleep;
	}

	cdev_init(&self->cdev, &besdbg_fops);
	self->cdev.owner = THIS_MODULE;
	ret = cdev_add(&self->cdev, self->major, 1);
	if (ret) {
		dev_err(dev, "can't add cdev");
		goto err_unreg_chrev_region;
	}

	struct device *sdev = device_create(besdbg_class, dev, self->major, self, "besdbg");
	if (IS_ERR(sdev)) {
		ret = PTR_ERR(sdev);
		goto err_cdev;
	}

/*
	irq = irq_of_parse_and_map(np, 0);
	if (!irq) {
		dev_warn(dev, "No irq in platform data\n");
	} else {
		global_plat_data->irq = irq;
	}
*/

	sdio_set_drvdata(func, self);
	sdio_claim_host(func);
	ret = sdio_enable_func(func);
	if (ret)
		dev_warn(dev, "can't enable func %d\n", ret);
	sdio_release_host(func);

	dev_info(dev, "probe success\n");

	return 0;

err_cdev:
	cdev_del(&self->cdev);
err_unreg_chrev_region:
	unregister_chrdev(self->major, "besdbg");
err_sleep:
	gpiod_direction_output(self->wakeup_device_gpio, 0);

	return ret;
}

static void besdbg_disconnect(struct sdio_func *func)
{
	struct besdbg_priv *self = sdio_get_drvdata(func);

	if (!self)
		return;

	cdev_del(&self->cdev);
	unregister_chrdev(self->major, "besdbg");
	device_destroy(besdbg_class, self->major);

	sdio_claim_host(func);
	sdio_disable_func(func);
	sdio_release_host(func);

	sdio_set_drvdata(func, NULL);

	gpiod_direction_output(self->wakeup_device_gpio, 0);
}

static const struct sdio_device_id besdbg_ids[] = {
	{ SDIO_DEVICE(0xbe57, 0x2002), },
	{ },
};

static struct sdio_driver besdbg_sdio_driver = {
	.name		= "besdbg_sdio",
	.id_table	= besdbg_ids,
	.probe		= besdbg_probe,
	.remove		= besdbg_disconnect,
};

//module_sdio_driver(besdbg_sdio_driver);

static int __init besdbg_driver_init(void)
{
	int ret;

	besdbg_class = class_create("besdbg");
	if (IS_ERR(besdbg_class))
		return PTR_ERR(besdbg_class);

	ret = sdio_register_driver(&besdbg_sdio_driver);
	if (ret) {
		class_destroy(besdbg_class);
		return ret;
	}

	return 0;
}

static void __exit besdbg_driver_exit(void)
{
	sdio_unregister_driver(&besdbg_sdio_driver);
	class_destroy(besdbg_class);
}

module_init(besdbg_driver_init);
module_exit(besdbg_driver_exit);

MODULE_AUTHOR("Ondrej Jirman <megi@xff.cz>");
MODULE_DESCRIPTION("BES2600 SDIO debug driver");
MODULE_LICENSE("GPL");
