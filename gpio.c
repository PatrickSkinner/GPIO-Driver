/**
 * File: gpio.c
 * Date: 04/08/2016
 * Author: Zhiyi Huang
 * Version: 0.2
 *
 * This is a gpio API for the dummy gpio device which
 * generates an interrupt for each half-byte (the most significant
 * bits are generated first.
 *
 * COSC440 assignment 2 in 2016.
 */

/* This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/version.h>
#include <linux/delay.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 3, 0)
        #include <asm/switch_to.h>
#else
        #include <asm/system.h>
#endif
#include <mach/platform.h>

static u32 gpio_dummy_base;


/* Define GPIO pins for the dummy device */
static struct gpio gpio_dummy[] = {
                { 7, GPIOF_IN, "GPIO7" },
                { 8, GPIOF_OUT_INIT_HIGH, "GPIO8" },
                { 17, GPIOF_IN, "GPIO17" },
                { 18, GPIOF_OUT_INIT_HIGH, "GPIO18" },
                { 22, GPIOF_IN, "GPIO22" },
                { 23, GPIOF_OUT_INIT_HIGH, "GPIO23" },
                { 24, GPIOF_IN, "GPIO24" },
                { 25, GPIOF_OUT_INIT_HIGH, "GPIO25" },
                { 4, GPIOF_OUT_INIT_LOW, "GPIO4" },
                { 27, GPIOF_IN, "GPIO27" },
};

static int dummy_irq;
extern irqreturn_t dummyport_interrupt(int irq, void *dev_id);

static inline u32
gpio_inw(u32 addr)
{
    u32 data;

    asm volatile("ldr %0,[%1]" : "=r"(data) : "r"(addr));
    return data;
}

static inline void
gpio_outw(u32 addr, u32 data)
{
    asm volatile("str %1,[%0]" : : "r"(addr), "r"(data));
}

void setgpiofunc(u32 func, u32 alt)
{
        u32 sel, data, shift;

        if(func > 53) return;
        sel = 0;
        while (func > 10) {
            func = func - 10;
            sel++;
        }
	sel = (sel << 2) + gpio_dummy_base;
        data = gpio_inw(sel);
        shift = func + (func << 1);
        data &= ~(7 << shift);
        data |= alt << shift;
        gpio_outw(sel, data);
}

u8 read_half_byte(void)
{
u32 c;
u8 r;

r = 0;
c = gpio_inw(gpio_dummy_base + 0x34);
if (c & (1 << 7)) r |= 1;
if (c & (1 << 17)) r |= 2;
if (c & (1 << 22)) r |= 4;
if (c & (1 << 24)) r |= 8;

return r;
}

static void write_to_gpio(char c)
{
volatile unsigned *gpio_set, *gpio_clear;

gpio_set = (unsigned *)((char *)gpio_dummy_base + 0x1c);
gpio_clear = (unsigned *)((char *)gpio_dummy_base + 0x28);

if(c & 1) *gpio_set = 1 << 8;
else *gpio_clear = 1 << 8;
udelay(1);

if(c & 2) *gpio_set = 1 << 18;
else *gpio_clear = 1 << 18;
udelay(1);

if(c & 4) *gpio_set = 1 << 23;
else *gpio_clear = 1 << 23;
udelay(1);

if(c & 8) *gpio_set = 1 << 25;
else *gpio_clear = 1 << 25;
udelay(1);

}

int gpio_dummy_init(void)
{
    int ret;

    gpio_dummy_base = (u32)ioremap_nocache(BCM2708_PERI_BASE + 0x200000, 4096);
    printk(KERN_WARNING "The gpio base is mapped to %x\n", gpio_dummy_base);
    ret = gpio_request_array(gpio_dummy, ARRAY_SIZE(gpio_dummy));

    if (ret) {
	printk(KERN_ERR "Unable to request GPIOs for the dummy device: %d\n", ret);
	goto fail2;
        }
    ret = gpio_to_irq(gpio_dummy[ARRAY_SIZE(gpio_dummy)-1].gpio);
    if(ret < 0) {
	printk(KERN_ERR "Unable to request IRQ for gpio %d: %d\n", gpio_dummy[ARRAY_SIZE(gpio_dummy)-1].gpio, ret);
        goto fail1;
    }
    dummy_irq = ret;
    printk(KERN_WARNING "Successfully requested IRQ# %d for %s\n", dummy_irq, gpio_dummy[ARRAY_SIZE(gpio_dummy)-1].label);

    ret = request_irq(dummy_irq, dummyport_interrupt, IRQF_TRIGGER_RISING | IRQF_ONESHOT, "gpio27", NULL);

    if(ret) {
	printk(KERN_ERR "Unable to request IRQ for dummy device: %d\n", ret);
	goto fail1;
    }
    write_to_gpio(15);
return 0;

fail1:
    gpio_free_array(gpio_dummy, ARRAY_SIZE(gpio_dummy));
fail2:
    iounmap((void *)gpio_dummy_base);
    return ret;
}

void gpio_dummy_exit(void)
{
    free_irq(dummy_irq, NULL);
    gpio_free_array(gpio_dummy, ARRAY_SIZE(gpio_dummy));
    iounmap((void *)gpio_dummy_base);
}
