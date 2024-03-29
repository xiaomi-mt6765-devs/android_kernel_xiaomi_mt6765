/*
 * Copyright (c) 2015 Fingerprint Cards AB <tech@fingerprints.com>
 * Copyright (C) 2020 XiaoMi, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License Version 2
 * as published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>

#include <teei_fp.h>
#include "tee_client_api.h"

#define FPC1022_RESET_LOW_US 5000
#define FPC1022_RESET_HIGH1_US 100

#define FPC_TTW_HOLD_TIME 3000

#define FPC102X_REG_HWID 252
#define FPC1022_CHIP 0x600
#define FPC1022_CHIP_MASK_SENSOR_TYPE 0xff00

#define GPIO_GET(pin) __gpio_get_value(pin)

extern void mt_spi_enable_master_clk(struct spi_device *spidev);
extern void mt_spi_disable_master_clk(struct spi_device *spidev);

struct TEEC_UUID uuid_ta_fpc = {0x7778C03F, 0xC30C, 0x4DD0,{0xA3, 0x19, 0xEA, 0x29, 0x64, 0x3D, 0x4D, 0x4B}};

struct fpc1022_data {
	struct device *dev;
	struct platform_device *pldev;
	struct spi_device *spi;
	int irq_gpio;
	int irq_num;

	struct pinctrl *pinctrl;
	struct pinctrl_state *st_irq;    //xpt
	struct pinctrl_state *st_rst_l;
	struct pinctrl_state *st_rst_h;

	struct input_dev *idev;
	char idev_name[32];
	int event_type;
	int event_code;
	struct mutex lock;
	bool prepared;
	bool wakeup_enabled;
	struct wakeup_source *ttw_wl;
};

extern bool goodix_fp_exist;
extern struct spi_device *spi_fingerprint;
bool fpc1022_fp_exist = false;

static struct fpc1022_data *fpc1022;
static int check_hwid(struct spi_device *spi);

u32 spi_speed = 1000000;

static void fpc1022_get_irqNum(struct fpc1022_data *p_data) {
	struct device_node *node;

	printk("%s\n", __func__);

	node = of_find_compatible_node(NULL, NULL, "mediatek,fpc1022_irq");

	if (node) {
		p_data->irq_num = irq_of_parse_and_map(node, 0);
		p_data->irq_gpio = of_get_named_gpio(node, "fpc,gpio_irq", 0);
		printk("%s , p_data->irq_num = %d, p_data->irq_gpio = %d\n",
		       __func__, p_data->irq_num, p_data->irq_gpio);
		if (!p_data->irq_num) {
			printk("irq_of_parse_and_map fail!!\n");
		}
	} else
		pr_err("%s can't find compatible node\n", __func__);
}

static int hw_reset(struct fpc1022_data *p_data) {
	struct device *dev = p_data->dev;

	pinctrl_select_state(p_data->pinctrl, p_data->st_rst_h);
	usleep_range(FPC1022_RESET_HIGH1_US, FPC1022_RESET_HIGH1_US + 100);

	pinctrl_select_state(p_data->pinctrl, p_data->st_rst_l);
	usleep_range(FPC1022_RESET_LOW_US, FPC1022_RESET_LOW_US + 100);

	pinctrl_select_state(p_data->pinctrl, p_data->st_rst_h);
	usleep_range(FPC1022_RESET_HIGH1_US, FPC1022_RESET_HIGH1_US + 100);

	dev_info(dev, "IRQ after reset %d\n", GPIO_GET(p_data->irq_gpio));

	return 0;
}

static ssize_t hw_reset_set(struct device *dev,
                            struct device_attribute *attr, const char *buf,
                            size_t count) {
	struct fpc1022_data *p_data = dev_get_drvdata(dev);

	if (!strncmp(buf, "reset", strlen("reset"))) {
		hw_reset(p_data);
	} else {
		return -EINVAL;
	}

	return count;
}

static DEVICE_ATTR(hw_reset, 0200, NULL, hw_reset_set);

/**
* sysfs node for controlling whether the driver is allowed
* to wake up the platform on interrupt.
*/
static ssize_t wakeup_enable_set(struct device *dev,
                                 struct device_attribute *attr, const char *buf,
                                 size_t count) {
	struct fpc1022_data *p_data = dev_get_drvdata(dev);

	if (!strncmp(buf, "enable", strlen("enable"))) {
		p_data->wakeup_enabled = true;
		smp_wmb();
	} else if (!strncmp(buf, "disable", strlen("disable"))) {
		p_data->wakeup_enabled = false;
		smp_wmb();
	} else {
		return -EINVAL;
	}

	return count;
}

static DEVICE_ATTR(wakeup_enable, 0200, NULL, wakeup_enable_set);

/**
* sysfs node for sending event to make the system interactive,
* i.e. waking up
*/
static ssize_t do_wakeup_set(struct device *dev,
                             struct device_attribute *attr, const char *buf,
                             size_t count) {
	struct fpc1022_data *p_data = dev_get_drvdata(dev);

	if (count > 0) {
		/* Sending power key event creates a toggling
		   effect that may be desired. It could be
		   replaced by another event such as KEY_WAKEUP. */
		input_report_key(p_data->idev, KEY_POWER, 1);
		input_report_key(p_data->idev, KEY_POWER, 0);
		input_sync(p_data->idev);
	} else {
		return -EINVAL;
	}

	return count;
}

static DEVICE_ATTR(do_wakeup, 0200, NULL, do_wakeup_set);

static ssize_t clk_enable_set(struct device *dev,
                              struct device_attribute *attr, const char *buf,
                              size_t count) {
	struct fpc1022_data *p_data = dev_get_drvdata(dev);
	dev_dbg(p_data->dev, " buff is %d, %s\n", *buf, __func__);

	if (!(p_data->spi)) {
		dev_err(p_data->dev, " spi clk NULL%s\n", __func__);
		return 0;
	}
	if (*buf == 49) {
		mt_spi_enable_master_clk(p_data->spi);
		dev_err(p_data->dev, " enable spi clk %s\n", __func__);
		return 1;
	}
	if (*buf == 48) {
		mt_spi_disable_master_clk(p_data->spi);
		dev_err(p_data->dev, " disable spi clk %s\n", __func__);
		return 1;
	} else {
		dev_err(p_data->dev, " invalid spi clk parameter %s\n",
		        __func__);
		return 0;
	}
}

static DEVICE_ATTR(clk_enable, 0200, NULL, clk_enable_set);

/**
* sysf node to check the interrupt status of the sensor, the interrupt
* handler should perform sysf_notify to allow userland to poll the node.
*/
static ssize_t irq_get(struct device *device,
                       struct device_attribute *attribute, char *buffer) {
	struct fpc1022_data *p_data = dev_get_drvdata(device);
	int irq = __gpio_get_value(p_data->irq_gpio);
	return scnprintf(buffer, PAGE_SIZE, "%i\n", irq);
}

/**
* writing to the irq node will just drop a printk message
* and return success, used for latency measurement.
*/
static ssize_t irq_ack(struct device *device,
                       struct device_attribute *attribute,
                       const char *buffer, size_t count) {
	struct fpc1022_data *p_data = dev_get_drvdata(device);
	dev_dbg(p_data->dev, "%s\n", __func__);
	return count;
}

static DEVICE_ATTR(irq, 0600, irq_get, irq_ack);

static ssize_t fpc_ic_is_exist(struct device *device,
                               struct device_attribute *attribute, char *buffer) {
	int fpc_exist;

	if (fpc1022_fp_exist) {
		fpc_exist = 1;
	} else {
		fpc_exist = 0;
	}
	return scnprintf(buffer, PAGE_SIZE, "%i\n", fpc_exist);
}

static DEVICE_ATTR(fpid_get, 0600, fpc_ic_is_exist, NULL);

static struct attribute *attributes[] = {
	&dev_attr_hw_reset.attr,
	&dev_attr_wakeup_enable.attr,
	&dev_attr_do_wakeup.attr,
	&dev_attr_clk_enable.attr,
	&dev_attr_irq.attr,
	&dev_attr_fpid_get.attr,
	NULL
};

static const struct attribute_group attribute_group = {
	.attrs = attributes,
};

static irqreturn_t fpc1022_irq_handler(int irq, void *handle) {
	struct fpc1022_data *p_data = handle;

	/* Make sure 'wakeup_enabled' is updated before using it
	 ** since this is interrupt context (other thread...) */
	smp_rmb();

	__pm_wakeup_event(p_data->ttw_wl, msecs_to_jiffies(FPC_TTW_HOLD_TIME));
	sysfs_notify(&p_data->dev->kobj, NULL, dev_attr_irq.attr.name);

	return IRQ_HANDLED;
}

static int fpc1022_platform_probe(struct platform_device *pldev) {
	long ret;
	int irqf;
	u32 val;
	const char *idev_name;
	struct device *dev = &pldev->dev;
	struct device_node *np = dev->of_node;

	dev_info(dev, "%s\n", __func__);

	if (!np) {
		dev_err(dev, "no of node found\n");
		ret = -EINVAL;
		goto err_no_of_node;
	}

	fpc1022 = devm_kzalloc(dev, sizeof(*fpc1022), GFP_KERNEL);
	if (!fpc1022) {
		dev_err(dev,
		        "failed to allocate memory for struct fpc1022_data\n");
		ret = -ENOMEM;
		goto err_fpc1022_malloc;
	}

	if (spi_fingerprint == NULL) {
		pr_notice("%s Line:%d spi device is NULL,cannot spi transfer\n",
		          __func__, __LINE__);
	} else {
		ret = check_hwid(spi_fingerprint);
		if (ret < 0) {
			pr_notice("%s: %d get chipid fail. now exit\n",
			          __func__, __LINE__);
			return -EINVAL;
		}
		fpc1022_fp_exist = true;

		fpc1022->spi = spi_fingerprint;
		memcpy(&uuid_fp, &uuid_ta_fpc, sizeof(struct TEEC_UUID));
	}

	fpc1022->dev = dev;
	dev_set_drvdata(dev, fpc1022);
	fpc1022->pldev = pldev;

	fpc1022->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(fpc1022->pinctrl)) {
		dev_err(dev, "Cannot find pinctrl!");
		ret = PTR_ERR(fpc1022->pinctrl);
		goto err_pinctrl_get;
	}

	fpc1022->st_irq = pinctrl_lookup_state(fpc1022->pinctrl, "default");
	if (IS_ERR(fpc1022->st_irq)) {
		ret = PTR_ERR(fpc1022->st_irq);
		dev_err(dev, "pinctrl err, irq\n");
	}

	fpc1022->st_rst_h =
		pinctrl_lookup_state(fpc1022->pinctrl, "reset_high");
	if (IS_ERR(fpc1022->st_rst_h)) {
		ret = PTR_ERR(fpc1022->st_rst_h);
		dev_err(dev, "pinctrl err, rst_high\n");
		goto err_lookup_state;
	}

	fpc1022->st_rst_l = pinctrl_lookup_state(fpc1022->pinctrl, "reset_low");
	if (IS_ERR(fpc1022->st_rst_l)) {
		ret = PTR_ERR(fpc1022->st_rst_l);
		dev_err(dev, "pinctrl err, rst_low\n");
		goto err_lookup_state;
	}
	fpc1022_get_irqNum(fpc1022);

	ret = of_property_read_u32(np, "fpc,event-type", &val);
	fpc1022->event_type = ret < 0 ? EV_MSC : val;

	ret = of_property_read_u32(np, "fpc,event-code", &val);
	fpc1022->event_code = ret < 0 ? MSC_SCAN : val;

	fpc1022->idev = devm_input_allocate_device(dev);
	if (!fpc1022->idev) {
		dev_err(dev, "failed to allocate input device\n");
		ret = -ENOMEM;
		goto err_input_malloc;
	}

	input_set_capability(fpc1022->idev, fpc1022->event_type,
	                     fpc1022->event_code);

	if (!of_property_read_string(np, "input-device-name", &idev_name)) {
		fpc1022->idev->name = idev_name;
	} else {
		snprintf(fpc1022->idev_name, sizeof(fpc1022->idev_name),
		         "fpc_irq@%s", dev_name(dev));
		fpc1022->idev->name = fpc1022->idev_name;
	}

	/* Also register the key for wake up */
	set_bit(EV_KEY, fpc1022->idev->evbit);
	set_bit(EV_PWR, fpc1022->idev->evbit);
	set_bit(KEY_WAKEUP, fpc1022->idev->keybit);
	set_bit(KEY_POWER, fpc1022->idev->keybit);
	ret = input_register_device(fpc1022->idev);
	fpc1022->wakeup_enabled = false;

	if (ret) {
		dev_err(dev, "failed to register input device\n");
		goto err_register_input;
	}

	irqf = IRQF_TRIGGER_RISING | IRQF_ONESHOT;
	if (of_property_read_bool(dev->of_node, "fpc,enable-wakeup")) {
		irqf |= IRQF_NO_SUSPEND;
		device_init_wakeup(dev, 1);
	}

	mutex_init(&fpc1022->lock);

	ret = devm_request_threaded_irq(dev, fpc1022->irq_num,
	                                NULL, fpc1022_irq_handler, irqf,
	                                dev_name(dev), fpc1022);

	if (ret) {
		dev_err(dev, "could not request irq %d\n", fpc1022->irq_num);
		goto err_request_irq;
	}
	dev_info(dev, "requested irq %d\n", fpc1022->irq_num);

	/* Request that the interrupt should be wakeable */
	enable_irq_wake(fpc1022->irq_num);
	fpc1022->ttw_wl = wakeup_source_register("fpc_ttw_wl");

	ret = sysfs_create_group(&dev->kobj, &attribute_group);
	if (ret) {
		dev_err(dev, "could not create sysfs\n");
		goto err_create_sysfs;
	}

	hw_reset(fpc1022);
	dev_info(dev, "%s: ok\n", __func__);

	return ret;

err_create_sysfs:
	wakeup_source_unregister(fpc1022->ttw_wl);

err_request_irq:
	mutex_destroy(&fpc1022->lock);

err_register_input:
	input_unregister_device(fpc1022->idev);

err_input_malloc:
err_lookup_state:
err_pinctrl_get:
	devm_kfree(dev, fpc1022);

err_fpc1022_malloc:
err_no_of_node:

	return ret;
}

static int fpc1022_platform_remove(struct platform_device *pldev) {
	struct device *dev = &pldev->dev;
	struct fpc1022_data *p_data = dev_get_drvdata(dev);

	dev_info(dev, "%s\n", __func__);

	sysfs_remove_group(&dev->kobj, &attribute_group);

	mutex_destroy(&p_data->lock);
	wakeup_source_unregister(p_data->ttw_wl);

	input_unregister_device(p_data->idev);
	devm_kfree(dev, p_data);

	return 0;
}

static struct of_device_id fpc1022_of_match[] = {{.compatible = "mediatek,fpc1022_irq"}};
MODULE_DEVICE_TABLE(of, fpc1022_of_match);

static struct platform_driver fpc1022_driver = {
	.driver = {
		.name = "fpc1022_irq",
		.owner = THIS_MODULE,
		.of_match_table = fpc1022_of_match
	},
	.probe = fpc1022_platform_probe,
	.remove = fpc1022_platform_remove
};

static int spi_read_hwid(struct spi_device *spi, u8 *rx_buf) {
	int error;
	struct spi_message msg;
	struct spi_transfer *xfer;
	u8 tmp_buf[10] = {0};
	u8 addr = FPC102X_REG_HWID;

	xfer = kzalloc(sizeof(*xfer) * 2, GFP_KERNEL);
	if (xfer == NULL) {
		dev_err(&spi->dev, "%s, no memory for SPI transfer\n",
		        __func__);
		return -ENOMEM;
	}

	spi_message_init(&msg);

	tmp_buf[0] = addr;
	xfer[0].tx_buf = tmp_buf;
	xfer[0].len = 1;
	xfer[0].delay_usecs = 5;

#ifdef CONFIG_SPI_MT65XX
	xfer[0].speed_hz = spi_speed;
	pr_err("%s %d, now spi-clock:%d\n",
	       __func__, __LINE__, xfer[0].speed_hz);
#endif

	spi_message_add_tail(&xfer[0], &msg);

	xfer[1].tx_buf = tmp_buf + 2;
	xfer[1].rx_buf = tmp_buf + 4;
	xfer[1].len = 2;
	xfer[1].delay_usecs = 5;

#ifdef CONFIG_SPI_MT65XX
	xfer[1].speed_hz = spi_speed;
#endif

	spi_message_add_tail(&xfer[1], &msg);
	error = spi_sync(spi, &msg);
	if (error) {
		dev_err(&spi->dev, "%s : spi_sync failed.\n", __func__);
	}

	memcpy(rx_buf, (tmp_buf + 4), 2);

	kfree(xfer);
	xfer = NULL;

	return 0;
}

static int check_hwid(struct spi_device *spi) {
	int error;
	u32 time_out = 0;
	u8 tmp_buf[2] = {0};
	u16 hardware_id;

	do {
		spi_read_hwid(spi, tmp_buf);
		printk(KERN_INFO "%s, fpc1022 chip version is 0x%x, 0x%x\n",
		       __func__, tmp_buf[0], tmp_buf[1]);

		time_out++;

		hardware_id = ((tmp_buf[0] << 8) | (tmp_buf[1]));
		pr_err("fpc hardware_id[0]= 0x%x id[1]=0x%x\n", tmp_buf[0],
		       tmp_buf[1]);

		if ((FPC1022_CHIP_MASK_SENSOR_TYPE & hardware_id) ==
		    FPC1022_CHIP) {
			pr_err("fpc match hardware_id = 0x%x is true\n",
			       hardware_id);
			error = 0;
		} else {
			pr_err("fpc match hardware_id = 0x%x is failed\n",
			       hardware_id);
			error = -1;
		}

		if (!error) {
			printk(KERN_INFO
			       "fpc %s, fpc1022 chip version check pass, time_out=%d\n",
			       __func__, time_out);
			return 0;
		}
	} while (time_out < 2);

	printk(KERN_INFO "%s, fpc1022 chip version read failed, time_out=%d\n",
	       __func__, time_out);

	return -1;
}

MODULE_DEVICE_TABLE(of, fpc1022_spi_of_match);

static int __init fpc1022_init(void) {
	printk(KERN_INFO "%s\n", __func__);

	if (goodix_fp_exist) {
		pr_err
		("%s goodix sensor has been detected, so exit FPC sensor detect.\n",
		 __func__);
		return -EINVAL;
	}

	if (0 != platform_driver_register(&fpc1022_driver)) {
		printk(KERN_INFO "%s: register platform driver fail\n",
		       __func__);
		return -EINVAL;
	} else {
		printk(KERN_INFO "%s: register platform driver success\n",
		       __func__);
	}

	return 0;
}

static void __exit fpc1022_exit(void) {
	printk(KERN_INFO "%s\n", __func__);

	platform_driver_unregister(&fpc1022_driver);
}

late_initcall(fpc1022_init);
module_exit(fpc1022_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Aleksej Makarov");
MODULE_AUTHOR("Henrik Tillman <henrik.tillman@fingerprints.com>");
MODULE_DESCRIPTION("fpc1022 Fingerprint sensor device driver.");
