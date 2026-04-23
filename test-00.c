/* #include <linux/init.h> */
#include <linux/kernel.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");

int init_module(void) {
    printk(KERN_ALERT "Hello, world 1.\n");

    return 0;
}

void cleanup_module(void) {
    printk(KERN_ALERT "Goodbye, world 1.\n");
}
