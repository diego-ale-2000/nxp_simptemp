#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/workqueue.h>

#define DRIVER_NAME "nxp_simtemp"
#define DEV_NAME    "simtemp"

struct nxp_simtemp_dev {
    struct miscdevice misc;
    struct hrtimer timer;
    struct work_struct work;
    unsigned int sampling_ms;
    s32 threshold_mC;
    bool running;
};

static struct nxp_simtemp_dev *gdev;

static void simtemp_work_func(struct work_struct *work)
{
    struct nxp_simtemp_dev *dev = container_of(work, struct nxp_simtemp_dev, work);
    int temp_mC = 40000 + (get_random_u32() % 8000) - 4000; // 40°C ±4°C
    pr_info(DRIVER_NAME ": new sample = %d m°C\n", temp_mC);
}

static enum hrtimer_restart simtemp_timer_cb(struct hrtimer *t)
{
    struct nxp_simtemp_dev *dev = container_of(t, struct nxp_simtemp_dev, timer);
    if (!dev->running)
        return HRTIMER_NORESTART;
    schedule_work(&dev->work);
    hrtimer_forward_now(&dev->timer, ms_to_ktime(dev->sampling_ms));
    return HRTIMER_RESTART;
}

static int simtemp_open(struct inode *inode, struct file *filp)
{
    filp->private_data = gdev;
    return 0;
}

static const struct file_operations simtemp_fops = {
    .owner = THIS_MODULE,
    .open  = simtemp_open,
};

static int __init nxp_simtemp_init(void)
{
    int ret;
    gdev = kzalloc(sizeof(*gdev), GFP_KERNEL);
    if (!gdev)
        return -ENOMEM;

    INIT_WORK(&gdev->work, simtemp_work_func);
    gdev->sampling_ms = 1000;
    gdev->running = true;

    hrtimer_init(&gdev->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    gdev->timer.function = simtemp_timer_cb;
    hrtimer_start(&gdev->timer, ms_to_ktime(gdev->sampling_ms), HRTIMER_MODE_REL);

    gdev->misc.minor = MISC_DYNAMIC_MINOR;
    gdev->misc.name  = DEV_NAME;
    gdev->misc.fops  = &simtemp_fops;
    ret = misc_register(&gdev->misc);
    if (ret)
        return ret;

    pr_info(DRIVER_NAME ": /dev/%s ready\n", DEV_NAME);
    return 0;
}

static void __exit nxp_simtemp_exit(void)
{
    gdev->running = false;
    hrtimer_cancel(&gdev->timer);
    cancel_work_sync(&gdev->work);
    misc_deregister(&gdev->misc);
    kfree(gdev);
    pr_info(DRIVER_NAME ": unregistered\n");
}

module_init(nxp_simtemp_init);
module_exit(nxp_simtemp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Diego Delgado");
MODULE_DESCRIPTION("NXP simulated temperature sensor");
MODULE_VERSION("0.1");
