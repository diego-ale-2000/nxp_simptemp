#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Diego Delgado");
MODULE_DESCRIPTION("Mini test kernel module");
MODULE_VERSION("0.1");

static int __init simtemp_init(void){
    printk(KERN_INFO "nxp_simtemp: Hello, kernel!\n");
    return 0; 
}

static void __exit simtemp_exit(void){
    printk(KERN_INFO "nxp_simtemp: Goodbye, kernel!\n");
}

module_init(simtemp_init);
module_exit(simtemp_exit);
