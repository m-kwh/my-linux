/*
 * LED Class Core
 *
 * Copyright (C) 2005 John Lenz <lenz@cs.wisc.edu>
 * Copyright (C) 2005-2007 Richard Purdie <rpurdie@openedhand.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *///此子系统用于：1：创建leds类，2：创建brightness，max_brightness等属性，3：提供register接口
//4.cat或者echo属性文件
#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include "leds.h"

static struct class *leds_class;//创建一个类结构体变量
//当用户cat /sys/class/leds/xxx/brightness时会调用led-class.c中的brightness_show函数
static ssize_t brightness_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);

	/* no lock needed for this */
	led_update_brightness(led_cdev);

	return sprintf(buf, "%u\n", led_cdev->brightness);
}
//当用户echo 100 > /sys/class/leds/xxx/brightness时会调用led-class.c中的brightness_store函数
static ssize_t brightness_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	unsigned long state;
	ssize_t ret;

	mutex_lock(&led_cdev->led_access);

	if (led_sysfs_is_disabled(led_cdev)) {
		ret = -EBUSY;
		goto unlock;
	}

	ret = kstrtoul(buf, 10, &state);//读取用户传入的brightness值,并存入state中
	if (ret)
		goto unlock;

	if (state == LED_OFF)//如果用户写入的brightness是0,即关闭led,则把对应的trigger移除
		led_trigger_remove(led_cdev);
	led_set_brightness(led_cdev, state);

	ret = size;
unlock:
	mutex_unlock(&led_cdev->led_access);
	return ret;
}
static DEVICE_ATTR_RW(brightness);

static ssize_t max_brightness_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", led_cdev->max_brightness);
}
static DEVICE_ATTR_RO(max_brightness);

#ifdef CONFIG_LEDS_TRIGGERS
static DEVICE_ATTR(trigger, 0644, led_trigger_show, led_trigger_store);
static struct attribute *led_trigger_attrs[] = {
	&dev_attr_trigger.attr,
	NULL,
};
static const struct attribute_group led_trigger_group = {
	.attrs = led_trigger_attrs,
};
#endif

static struct attribute *led_class_attrs[] = {
	&dev_attr_brightness.attr,//创建brightness，max_brightness属性
	&dev_attr_max_brightness.attr,
	NULL,
};

static const struct attribute_group led_group = {
	.attrs = led_class_attrs,
};

static const struct attribute_group *led_groups[] = {
	&led_group,
#ifdef CONFIG_LEDS_TRIGGERS //只有打开这个宏,才会创建对应的trigger属性
	&led_trigger_group,
#endif
	NULL,
};

static void led_timer_function(unsigned long data)
{
	struct led_classdev *led_cdev = (void *)data;
	unsigned long brightness;
	unsigned long delay;

	if (!led_cdev->blink_delay_on || !led_cdev->blink_delay_off) {
		led_set_brightness_async(led_cdev, LED_OFF);
		return;
	}

	if (led_cdev->flags & LED_BLINK_ONESHOT_STOP) {
		led_cdev->flags &= ~LED_BLINK_ONESHOT_STOP;
		return;
	}

	brightness = led_get_brightness(led_cdev);
	if (!brightness) {
		/* Time to switch the LED on. */
		brightness = led_cdev->blink_brightness;
		delay = led_cdev->blink_delay_on;
	} else {
		/* Store the current brightness value to be able
		 * to restore it when the delay_off period is over.
		 */
		led_cdev->blink_brightness = brightness;
		brightness = LED_OFF;
		delay = led_cdev->blink_delay_off;
	}

	led_set_brightness_async(led_cdev, brightness);

	/* Return in next iteration if led is in one-shot mode and we are in
	 * the final blink state so that the led is toggled each delay_on +
	 * delay_off milliseconds in worst case.
	 */
	if (led_cdev->flags & LED_BLINK_ONESHOT) {
		if (led_cdev->flags & LED_BLINK_INVERT) {
			if (brightness)
				led_cdev->flags |= LED_BLINK_ONESHOT_STOP;
		} else {
			if (!brightness)
				led_cdev->flags |= LED_BLINK_ONESHOT_STOP;
		}
	}

	mod_timer(&led_cdev->blink_timer, jiffies + msecs_to_jiffies(delay));
}

static void set_brightness_delayed(struct work_struct *ws)
{
	struct led_classdev *led_cdev =
		container_of(ws, struct led_classdev, set_brightness_work);

	led_stop_software_blink(led_cdev);

	led_set_brightness_async(led_cdev, led_cdev->delayed_set_value);
}

/**
 * led_classdev_suspend - suspend an led_classdev.
 * @led_cdev: the led_classdev to suspend.
 */
void led_classdev_suspend(struct led_classdev *led_cdev)
{
	led_cdev->flags |= LED_SUSPENDED;
	led_cdev->brightness_set(led_cdev, 0);
}
EXPORT_SYMBOL_GPL(led_classdev_suspend);

/**
 * led_classdev_resume - resume an led_classdev.
 * @led_cdev: the led_classdev to resume.
 */
void led_classdev_resume(struct led_classdev *led_cdev)
{
	led_cdev->brightness_set(led_cdev, led_cdev->brightness);

	if (led_cdev->flash_resume)
		led_cdev->flash_resume(led_cdev);

	led_cdev->flags &= ~LED_SUSPENDED;
}
EXPORT_SYMBOL_GPL(led_classdev_resume);

#ifdef CONFIG_PM_SLEEP
static int led_suspend(struct device *dev)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);

	if (led_cdev->flags & LED_CORE_SUSPENDRESUME)
		led_classdev_suspend(led_cdev);

	return 0;
}

static int led_resume(struct device *dev)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);

	if (led_cdev->flags & LED_CORE_SUSPENDRESUME)
		led_classdev_resume(led_cdev);

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(leds_class_dev_pm_ops, led_suspend, led_resume);

static int match_name(struct device *dev, const void *data)
{
	if (!dev_name(dev))
		return 0;
	return !strcmp(dev_name(dev), (char *)data);
}

static int led_classdev_next_name(const char *init_name, char *name,
				  size_t len)
{
	unsigned int i = 0;
	int ret = 0;
	struct device *dev;

	strlcpy(name, init_name, len);

	while ((ret < len) &&
	       (dev = class_find_device(leds_class, NULL, name, match_name))) {
		put_device(dev);
		ret = snprintf(name, len, "%s_%u", init_name, ++i);
	}

	if (ret >= len)
		return -ENOMEM;

	return i;
}

/**
 * led_classdev_register - register a new object of led_classdev class.
 * @parent: The device to register.
 * @led_cdev: the led_classdev structure for this device.
 *///在/sys/class/leds目录下创建xxx设备,并这个xxx目录下创建一系列attr属性文件,如brightness max_brightness trigger等
int led_classdev_register(struct device *parent, struct led_classdev *led_cdev)
{
	char name[64];
	int ret;
	//获取你要创建的设备的名字
	ret = led_classdev_next_name(led_cdev->name, name, sizeof(name));
	if (ret < 0)
		return ret;
	//基于leds这个类创建对应的设备
	led_cdev->dev = device_create_with_groups(leds_class, parent, 0,
				led_cdev, led_cdev->groups, "%s", name);
	if (IS_ERR(led_cdev->dev))
		return PTR_ERR(led_cdev->dev);

	if (ret)
		dev_warn(parent, "Led %s renamed to %s due to name collision",
				led_cdev->name, dev_name(led_cdev->dev));

#ifdef CONFIG_LEDS_TRIGGERS
	init_rwsem(&led_cdev->trigger_lock);
#endif
	mutex_init(&led_cdev->led_access);
	/* add to the list of leds */
	down_write(&leds_list_lock);
	list_add_tail(&led_cdev->node, &leds_list);
	up_write(&leds_list_lock);

	if (!led_cdev->max_brightness)
		led_cdev->max_brightness = LED_FULL;

	led_cdev->flags |= SET_BRIGHTNESS_ASYNC;

	led_update_brightness(led_cdev);

	INIT_WORK(&led_cdev->set_brightness_work, set_brightness_delayed);

	setup_timer(&led_cdev->blink_timer, led_timer_function,
		    (unsigned long)led_cdev);

#ifdef CONFIG_LEDS_TRIGGERS
	led_trigger_set_default(led_cdev);
#endif

	dev_dbg(parent, "Registered led device: %s\n",
			led_cdev->name);

	return 0;
}
EXPORT_SYMBOL_GPL(led_classdev_register);

/**
 * led_classdev_unregister - unregisters a object of led_properties class.
 * @led_cdev: the led device to unregister
 *
 * Unregisters a previously registered via led_classdev_register object.
 */
void led_classdev_unregister(struct led_classdev *led_cdev)
{
#ifdef CONFIG_LEDS_TRIGGERS
	down_write(&led_cdev->trigger_lock);
	if (led_cdev->trigger)
		led_trigger_set(led_cdev, NULL);
	up_write(&led_cdev->trigger_lock);
#endif

	cancel_work_sync(&led_cdev->set_brightness_work);

	/* Stop blinking */
	led_stop_software_blink(led_cdev);
	led_set_brightness(led_cdev, LED_OFF);

	device_unregister(led_cdev->dev);

	down_write(&leds_list_lock);
	list_del(&led_cdev->node);
	up_write(&leds_list_lock);

	mutex_destroy(&led_cdev->led_access);
}
EXPORT_SYMBOL_GPL(led_classdev_unregister);

static void devm_led_classdev_release(struct device *dev, void *res)
{
	led_classdev_unregister(*(struct led_classdev **)res);
}

/**
 * devm_led_classdev_register - resource managed led_classdev_register()
 * @parent: The device to register.
 * @led_cdev: the led_classdev structure for this device.
 */
int devm_led_classdev_register(struct device *parent,
			       struct led_classdev *led_cdev)
{
	struct led_classdev **dr;
	int rc;

	dr = devres_alloc(devm_led_classdev_release, sizeof(*dr), GFP_KERNEL);
	if (!dr)
		return -ENOMEM;

	rc = led_classdev_register(parent, led_cdev);
	if (rc) {
		devres_free(dr);
		return rc;
	}

	*dr = led_cdev;
	devres_add(parent, dr);

	return 0;
}
EXPORT_SYMBOL_GPL(devm_led_classdev_register);

static int devm_led_classdev_match(struct device *dev, void *res, void *data)
{
	struct led_cdev **p = res;

	if (WARN_ON(!p || !*p))
		return 0;

	return *p == data;
}

/**
 * devm_led_classdev_unregister() - resource managed led_classdev_unregister()
 * @parent: The device to unregister.
 * @led_cdev: the led_classdev structure for this device.
 */
void devm_led_classdev_unregister(struct device *dev,
				  struct led_classdev *led_cdev)
{
	WARN_ON(devres_release(dev,
			       devm_led_classdev_release,
			       devm_led_classdev_match, led_cdev));
}
EXPORT_SYMBOL_GPL(devm_led_classdev_unregister);

static int __init leds_init(void)//模块入口函数，会在/sys/class下创建leds类
{
	leds_class = class_create(THIS_MODULE, "leds");
	if (IS_ERR(leds_class))
		return PTR_ERR(leds_class);
	leds_class->pm = &leds_class_dev_pm_ops;
	leds_class->dev_groups = led_groups;
	return 0;
}

static void __exit leds_exit(void)
{
	class_destroy(leds_class);
}

subsys_initcall(leds_init);//子系统初始化
module_exit(leds_exit);

MODULE_AUTHOR("John Lenz, Richard Purdie");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("LED Class Interface");
