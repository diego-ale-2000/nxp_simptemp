#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>

#define DRIVER_NAME "nxp_simtemp"
#define DEV_NAME    "simtemp"

static int simtemp_open(struct inode *inode, struct file *filp)
{
    pr_info(DRIVER_NAME ": device opened\n");
    return 0;
}

static const struct file_operations simtemp_fops = {
    .owner = THIS_MODULE,
    .open  = simtemp_open,
};

static struct miscdevice simtemp_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = DEV_NAME,
    .fops  = &simtemp_fops,
};

static int __init nxp_simtemp_init(void)
{
    int ret = misc_register(&simtemp_miscdev);
    if (ret)
        return ret;
    pr_info(DRIVER_NAME ": registered /dev/%s\n", DEV_NAME);
    return 0;
}

static void __exit nxp_simtemp_exit(void)
{
    misc_deregister(&simtemp_miscdev);
    pr_info(DRIVER_NAME ": unregistered\n");
}

module_init(nxp_simtemp_init);
module_exit(nxp_simtemp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Diego Delgado");
MODULE_DESCRIPTION("NXP simulated temperature sensor (skeleton)");
MODULE_VERSION("0.1");
