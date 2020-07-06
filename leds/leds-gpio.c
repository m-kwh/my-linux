/*
 * LEDs driver for GPIOs
 *
 * Copyright (C) 2007 8D Technologies inc.
 * Raphael Assenat <raph@8d.com>
 * Copyright (C) 2008 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

struct gpio_led_data {//LED的操作函数
	struct led_classdev cdev;//类
	struct gpio_desc *gpiod;//提供对应的gpioAPI函数
	struct work_struct work;//用于工作队列
	u8 new_level;
	u8 can_sleep;
	u8 blinking;
	int (*platform_gpio_blink_set)(struct gpio_desc *desc, int state,
			unsigned long *delay_on, unsigned long *delay_off);
};

static void gpio_led_work(struct work_struct *work)
{
	struct gpio_led_data *led_dat =
		container_of(work, struct gpio_led_data, work);

	if (led_dat->blinking) {
		led_dat->platform_gpio_blink_set(led_dat->gpiod,
					led_dat->new_level, NULL, NULL);
		led_dat->blinking = 0;
	} else
		gpiod_set_value_cansleep(led_dat->gpiod, led_dat->new_level);
}

static void gpio_led_set(struct led_classdev *led_cdev,
	enum led_brightness value)
{
	struct gpio_led_data *led_dat =
		container_of(led_cdev, struct gpio_led_data, cdev);
	int level;

	if (value == LED_OFF)
		level = 0;
	else
		level = 1;

	/* Setting GPIOs with I2C/etc requires a task context, and we don't
	 * seem to have a reliable way to know if we're already in one; so
	 * let's just assume the worst.
	 */
	if (led_dat->can_sleep) {
		led_dat->new_level = level;
		schedule_work(&led_dat->work);
	} else {
		if (led_dat->blinking) {
			led_dat->platform_gpio_blink_set(led_dat->gpiod, level,
							 NULL, NULL);
			led_dat->blinking = 0;
		} else
			gpiod_set_value(led_dat->gpiod, level);
	}
}

static int gpio_blink_set(struct led_classdev *led_cdev,
	unsigned long *delay_on, unsigned long *delay_off)
{
	struct gpio_led_data *led_dat =
		container_of(led_cdev, struct gpio_led_data, cdev);

	led_dat->blinking = 1;
	return led_dat->platform_gpio_blink_set(led_dat->gpiod, GPIO_LED_BLINK,
						delay_on, delay_off);
}

static int create_gpio_led(const struct gpio_led *template,
	struct gpio_led_data *led_dat, struct device *parent,
	int (*blink_set)(struct gpio_desc *, int, unsigned long *,
			 unsigned long *))//创建 LED 相关的 io，其实就是设置 LED 所使用的 io为输出之类的
{
	int ret, state;

	led_dat->gpiod = template->gpiod;
	if (!led_dat->gpiod) {
		/*
		 * This is the legacy code path for platform code that
		 * still uses GPIO numbers. Ultimately we would like to get
		 * rid of this block completely.
		 */
		unsigned long flags = 0;

		/* skip leds that aren't available */
		if (!gpio_is_valid(template->gpio)) {
			dev_info(parent, "Skipping unavailable LED gpio %d (%s)\n",
					template->gpio, template->name);
			return 0;
		}

		if (template->active_low)
			flags |= GPIOF_ACTIVE_LOW;//设置低电平有效

		ret = devm_gpio_request_one(parent, template->gpio, flags,
					    template->name);//申请一个gpio
		if (ret < 0)
			return ret;

		led_dat->gpiod = gpio_to_desc(template->gpio);//???
		if (IS_ERR(led_dat->gpiod))
			return PTR_ERR(led_dat->gpiod);
	}
	//初始化led_dat结构体
	led_dat->cdev.name = template->name;
	led_dat->cdev.default_trigger = template->default_trigger;
	led_dat->can_sleep = gpiod_cansleep(led_dat->gpiod);//此函数用于分辨此设备能进入睡眠，即不能再中断中使用
	led_dat->blinking = 0;
	if (blink_set) {
		led_dat->platform_gpio_blink_set = blink_set;
		led_dat->cdev.blink_set = gpio_blink_set;//77行，设置闪烁功能
	}
	led_dat->cdev.brightness_set = gpio_led_set;//48行，设置亮灭功能
	if (template->default_state == LEDS_GPIO_DEFSTATE_KEEP)
		state = !!gpiod_get_value_cansleep(led_dat->gpiod);//读取gpio，可睡眠的读取
	else
		state = (template->default_state == LEDS_GPIO_DEFSTATE_ON);//读取gpio默认状态，即0或1
	led_dat->cdev.brightness = state ? LED_FULL : LED_OFF;//根据状态来点亮或灭led
	if (!template->retain_state_suspended)
		led_dat->cdev.flags |= LED_CORE_SUSPENDRESUME;

	ret = gpiod_direction_output(led_dat->gpiod, state);//根据状态来设置gpio的电平，即亮灭led，可睡眠的设置
	if (ret < 0)
		return ret;

	INIT_WORK(&led_dat->work, gpio_led_work);

	return led_classdev_register(parent, &led_dat->cdev);//注册led_classdev
}

static void delete_gpio_led(struct gpio_led_data *led)
{
	led_classdev_unregister(&led->cdev);
	cancel_work_sync(&led->work);
}

struct gpio_leds_priv {
	int num_leds;//led灯个数
	struct gpio_led_data leds[];//24行，设备结构体
};

static inline int sizeof_gpio_leds_priv(int num_leds)
{
	return sizeof(struct gpio_leds_priv) +
		(sizeof(struct gpio_led_data) * num_leds);
}

static struct gpio_leds_priv *gpio_leds_create(struct platform_device *pdev)
{//从设备树节点中获取属性并设置gpio的输出进行亮灭
	struct device *dev = &pdev->dev;
	struct fwnode_handle *child;//device_node成员，子节点
	struct gpio_leds_priv *priv;
	int count, ret;
	struct device_node *np;

	count = device_get_child_node_count(dev);//统计子节点数量
	if (!count)
		return ERR_PTR(-ENODEV);

	priv = devm_kzalloc(dev, sizeof_gpio_leds_priv(count), GFP_KERNEL);
	if (!priv)
		return ERR_PTR(-ENOMEM);

	device_for_each_child_node(dev, child) {//for循环，遍历每个子节点，获取每个子节点信息
		struct gpio_led led = {};
		const char *state = NULL;

		led.gpiod = devm_get_gpiod_from_child(dev, NULL, child);//获取gpio信息
		if (IS_ERR(led.gpiod)) {
			fwnode_handle_put(child);
			ret = PTR_ERR(led.gpiod);
			goto err;
		}

		np = of_node(child);//获取子节点，从fwnode_handle中

		if (fwnode_property_present(child, "label")) {//读取子节点标号信息，有则返回1
			fwnode_property_read_string(child, "label", &led.name);//将标号信息作为LED名字/sys/class/leds目录下的名称
		} else {
			if (IS_ENABLED(CONFIG_OF) && !led.name && np)
				led.name = np->name;
			if (!led.name)
				return ERR_PTR(-EINVAL);
		}
		fwnode_property_read_string(child, "linux,default-trigger",
					    &led.default_trigger);//获取linux,default-trigger属性，将值赋给&led.default_trigger

		if (!fwnode_property_read_string(child, "default-state",
						 &state)) {//获取default-state属性
			if (!strcmp(state, "keep"))
				led.default_state = LEDS_GPIO_DEFSTATE_KEEP;
			else if (!strcmp(state, "on"))
				led.default_state = LEDS_GPIO_DEFSTATE_ON;
			else
				led.default_state = LEDS_GPIO_DEFSTATE_OFF;
		}

		if (fwnode_property_present(child, "retain-state-suspended"))
			led.retain_state_suspended = 1;

		ret = create_gpio_led(&led, &priv->leds[priv->num_leds++],
				      dev, NULL);//88,此函数设置led的io输出模式等
		if (ret < 0) {
			fwnode_handle_put(child);
			goto err;
		}
	}

	return priv;

err:
	for (count = priv->num_leds - 2; count >= 0; count--)
		delete_gpio_led(&priv->leds[count]);
	return ERR_PTR(ret);
}

static const struct of_device_id of_gpio_leds_match[] = {
	{ .compatible = "gpio-leds", },
	{},//一个标记，最后一个匹配项必须为空
};

MODULE_DEVICE_TABLE(of, of_gpio_leds_match);//声明

static int gpio_led_probe(struct platform_device *pdev)
{
	struct gpio_led_platform_data *pdata = dev_get_platdata(&pdev->dev);//获取一个平台设备
	struct gpio_leds_priv *priv;//156行，私有结构体，即设备结构体
	int i, ret = 0;

	if (pdata && pdata->num_leds) {//非设备树方式获取LED灯的gpio信息，即获取platform_device
		priv = devm_kzalloc(&pdev->dev,
				sizeof_gpio_leds_priv(pdata->num_leds),
					GFP_KERNEL);//内存分配函数
		if (!priv)
			return -ENOMEM;

		priv->num_leds = pdata->num_leds;
		for (i = 0; i < priv->num_leds; i++) {//遍历每个led灯
			ret = create_gpio_led(&pdata->leds[i],
					      &priv->leds[i],
					      &pdev->dev, pdata->gpio_blink_set);//创建 LED 相关的 io，其实就是设置 LED 所使用的 io为输出之类的
			if (ret < 0) {
				/* On failure: unwind the led creations */
				for (i = i - 1; i >= 0; i--)
					delete_gpio_led(&priv->leds[i]);
				return ret;
			}
		}
	} else {//采用设备树方式获取LED灯的gpio信息
		priv = gpio_leds_create(pdev);//167,从设备树节点中获取属性并设置gpio的输出进行亮灭
		if (IS_ERR(priv))
			return PTR_ERR(priv);
	}

	platform_set_drvdata(pdev, priv);//将设置保存到平台设备结构体platform_device中

	return 0;
}

static int gpio_led_remove(struct platform_device *pdev)
{
	struct gpio_leds_priv *priv = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < priv->num_leds; i++)
		delete_gpio_led(&priv->leds[i]);

	return 0;
}

static struct platform_driver gpio_led_driver = {
	.probe		= gpio_led_probe,
	.remove		= gpio_led_remove,
	.driver		= {
		.name	= "leds-gpio",//驱动名称，这也是一种匹配方式,在/sys/bus/platform/driver目录下
		.of_match_table = of_gpio_leds_match,
	},
};

module_platform_driver(gpio_led_driver);//注册驱动,相当于注册和删除驱动函数

MODULE_AUTHOR("Raphael Assenat <raph@8d.com>, Trent Piepho <tpiepho@freescale.com>");
MODULE_DESCRIPTION("GPIO LED driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:leds-gpio");
