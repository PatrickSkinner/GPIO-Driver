#include <linux/interrupt.h>

int gpio_dummy_init(void);
void gpio_dummy_exit(void);
irqreturn_t dummyport_interrupt(int irq, void *dev_id);
u8 read_half_byte(void);
