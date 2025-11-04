#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <asm/io.h>

/* https://elixir.bootlin.com/linux/v6.17.7/source/arch/arm/include/asm/io.h#L162 */
#ifndef CONFIG_ARM_DMA_MEM_BUFFERABLE
#  warning "io memory buffer barrier not working."
#endif 

#ifdef BUFSIZ
#error BUSIZ already defined
#endif 

#define BUFSIZ 256

/* driver/base.h */
#define KERNEL_BASE 0x0 /* 0xffff000000000000 */
#define MMIO_BASE   (KERNEL_BASE + 0x3F000000)
#define LOCAL_BASE  (KERNEL_BASE + 0x40000000)

/* driver/gpio.h */
#define GPIO_BASE (MMIO_BASE + 0x200000)
#define ADR_MASK (0x1ffff)

#define GPFSEL0   (GPIO_BASE + 0x00)
#define GPFSEL1   (GPIO_BASE + 0x04)
#define GPFSEL2   (GPIO_BASE + 0x08)
#define GPFSEL3   (GPIO_BASE + 0x0C)
#define GPFSEL4   (GPIO_BASE + 0x10)
#define GPFSEL5   (GPIO_BASE + 0x14)
#define GPSET0    (GPIO_BASE + 0x1C)
#define GPSET1    (GPIO_BASE + 0x20)
#define GPCLR0    (GPIO_BASE + 0x28)
#define GPLEV0    (GPIO_BASE + 0x34)
#define GPLEV1    (GPIO_BASE + 0x38)
#define GPEDS0    (GPIO_BASE + 0x40)
#define GPEDS1    (GPIO_BASE + 0x44)
#define GPHEN0    (GPIO_BASE + 0x64)
#define GPHEN1    (GPIO_BASE + 0x68)
#define GPPUD     (GPIO_BASE + 0x94)
#define GPPUDCLK0 (GPIO_BASE + 0x98)
#define GPPUDCLK1 (GPIO_BASE + 0x9C)

/* driver/aux.h */
#define AUX_BASE (MMIO_BASE + 0x215000)

#define AUX_ENABLES     (AUX_BASE + 0x04)
#define AUX_MU_IO_REG   (AUX_BASE + 0x40)
#define AUX_MU_IER_REG  (AUX_BASE + 0x44)
#define AUX_MU_IIR_REG  (AUX_BASE + 0x48)
#define AUX_MU_LCR_REG  (AUX_BASE + 0x4C)
#define AUX_MU_MCR_REG  (AUX_BASE + 0x50)
#define AUX_MU_LSR_REG  (AUX_BASE + 0x54)
#define AUX_MU_MSR_REG  (AUX_BASE + 0x58)
#define AUX_MU_SCRATCH  (AUX_BASE + 0x5C)
#define AUX_MU_CNTL_REG (AUX_BASE + 0x60)
#define AUX_MU_STAT_REG (AUX_BASE + 0x64)
#define AUX_MU_BAUD_REG (AUX_BASE + 0x68)

#define AUX_UART_CLOCK 250000000
#define AUX_MU_BAUD(baudrate) ((AUX_UART_CLOCK / ((baudrate)*8)) - 1)

typedef unsigned int u32;
static void __iomem *baseaddr;

static ssize_t pl010_read(struct file *file, char __user *buf, 
            size_t count, loff_t *offset);
static ssize_t pl010_write(struct file *file, const char __user *buf, 
            size_t count, loff_t *offset);

static unsigned long get_clock_frequency(void) {
    unsigned long result;
    asm volatile("mrs %[freq], cntfrq_el0" : [freq] "=r"(result));
    return result;
}

static void compiler_fence(void) {
    asm volatile("" :::"memory");
}

static unsigned long get_timestamp(void) {
    unsigned long result;
    compiler_fence();
    asm volatile("mrs %[cnt], cntpct_el0" : [cnt] "=r"(result));
    compiler_fence();
    return result;
}

static void delay_us(unsigned long n) {
   unsigned long freq = get_clock_frequency();
   unsigned long end = get_timestamp(), now;
   end += freq / 1000000 * n;

   do {
       now = get_timestamp();
   } while (now <= end);
}

static void device_put_u32(u32 addr, u32 value) {
   compiler_fence();
   writel(value, baseaddr + (addr & ADR_MASK));
   compiler_fence();
}

static u32 device_get_u32(u32 addr) {
   compiler_fence();
   u32 value = readl(baseaddr + (addr & ADR_MASK));
   compiler_fence();
   return value;
}

static void uart_init(void) {
    device_put_u32(GPPUD, 0);
    delay_us(5);
    device_put_u32(GPPUDCLK0, (1 << 14) | (1 << 15));
    delay_us(5);
    device_put_u32(GPPUDCLK0, 0);

    // enable mini uart and access to its registers.
    device_put_u32(AUX_ENABLES, 1);
    // disable auto flow control, receiver and transmitter (for now).
    device_put_u32(AUX_MU_CNTL_REG, 0);
    // enable receiving interrupts.
    device_put_u32(AUX_MU_IER_REG, 3 << 2 | 1);
    // enable 8-bit mode.
    device_put_u32(AUX_MU_LCR_REG, 3);
    // set RTS line to always high.
    device_put_u32(AUX_MU_MCR_REG, 0);
    // set baud rate to 115200.
    device_put_u32(AUX_MU_BAUD_REG, AUX_MU_BAUD(115200));
    // clear receive and transmit FIFO.
    device_put_u32(AUX_MU_IIR_REG, 6);
    // finally, enable receiver and transmitter.
    device_put_u32(AUX_MU_CNTL_REG, 3);
}

static char uart_get_char(void) {
    u32 state = device_get_u32(AUX_MU_IIR_REG);
    /*
     * if ((state & 1) || (state & 6) != 4)
     *    return -1;
     */

    while ((state & 1) || (state & 6) != 4) {
        state = device_get_u32(AUX_MU_IIR_REG);
    }
    return device_get_u32(AUX_MU_IO_REG) & 0xff;
}

static void uart_put_char(char c) {
    while (!(device_get_u32(AUX_MU_LSR_REG) & 0x20)) {}

    device_put_u32(AUX_MU_IO_REG, c);
    if (c == '\n') {
        uart_put_char('\r');
    }
}

static int dev_major = 0;
static struct cdev cdev;
static struct class *pl010_class = NULL;
static struct file_operations fops = {
   .owner = THIS_MODULE,
   .read = pl010_read,
   .write = pl010_write,
};

static int __init pl010_init(void) {
   baseaddr = ioremap(GPIO_BASE, ADR_MASK + 1);
   if (!baseaddr) {
     printk("pl010: cannot map gpio address");
     return -EINVAL;
   }
   uart_init();

   dev_t dev;
   int i = 0;

   /* allocate device range */
   alloc_chrdev_region(&dev, 0, 1, "pl010");

   /* create device major number */
   dev_major = MAJOR(dev);

   /* create class */
   pl010_class = class_create("pl010");

   /* register device */
   cdev_init(&cdev, &fops);
   cdev.owner = THIS_MODULE;
   cdev_add(&cdev, MKDEV(dev_major, i), 1);
   device_create(pl010_class, NULL, MKDEV(dev_major, i), NULL, "pl010");

   return i;
}

static void __exit pl010_exit(void) {
   device_destroy(pl010_class, MKDEV(dev_major, 0));
   class_unregister(pl010_class);
   class_destroy(pl010_class);
   unregister_chrdev_region(MKDEV(dev_major, 0), MINORMASK);
   iounmap(baseaddr);
}

static ssize_t pl010_read(struct file *file, char __user *addrspace, 
            size_t count, loff_t *offset) {
    char buf[BUFSIZ];
    
    size_t nr = BUFSIZ;    
    nr = nr > count ? count : nr;
    int pt = 0;
    
    while (nr != 0) {
        char ch = uart_get_char();
        uart_put_char(ch);
	if (ch == '\r') {
	    ch = '\n';
	    uart_put_char(ch);
	}
        buf[pt++]  = ch;
        if (ch == '\n') { break; }
	nr -= 1;
    }
    
    *offset += count - nr;
    bool pgfault =  copy_to_user(addrspace, buf, count - nr);
    return pgfault ? -EFAULT : count - nr;
}

static ssize_t pl010_write(struct file *file, const char __user *addrspace, 
            size_t count, loff_t *offset) {
    char buf[BUFSIZ];
    int error = 0;
    size_t nw = 0;

    while (nw < count) {
        size_t ncpy = count - nw;
        ncpy = ncpy > BUFSIZ ? BUFSIZ : ncpy;
        if (copy_from_user(buf, addrspace, ncpy)) {
            error = -EFAULT;
            break;
        }
        
        for (size_t i = 0; i < ncpy; i++) {
            uart_put_char(buf[i]);
        }
        nw += ncpy;
    }

    *offset += nw;
    return nw > 0 ? nw : error;
}

module_init(pl010_init);
module_exit(pl010_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Fudanyrd");

