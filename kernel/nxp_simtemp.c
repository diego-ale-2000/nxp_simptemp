#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/device.h>  // para sysfs

#define DRIVER_NAME "nxp_simtemp"
#define DEV_NAME    "simtemp"

struct simtemp_sample {
    __u64 timestamp_ns;
    __s32 temp_mC;
    __u32 flags;
} __attribute__((packed));

struct nxp_simtemp_dev {
    struct miscdevice misc;
    struct hrtimer timer;
    struct work_struct work;
    spinlock_t lock;
    wait_queue_head_t wq;
    struct simtemp_sample *buffer;
    unsigned int buf_size;
    unsigned int head;
    unsigned int tail;
    unsigned int sampling_ms;
    s32 threshold_mC;
    bool running;

    struct kobject *kobj;
};

static struct nxp_simtemp_dev *gdev;

/* ================== Sysfs handlers ================== */

static ssize_t sampling_ms_show(struct kobject *kobj,
                                struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%u\n", gdev->sampling_ms);
}

static ssize_t sampling_ms_store(struct kobject *kobj,
                                 struct kobj_attribute *attr,
                                 const char *buf, size_t count)
{
    unsigned int val;
    if (kstrtouint(buf, 10, &val))
        return -EINVAL;
    if (val == 0)
        return -EINVAL;

    gdev->sampling_ms = val;
    return count;
}

static ssize_t threshold_mC_show(struct kobject *kobj,
                                 struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", gdev->threshold_mC);
}

static ssize_t threshold_mC_store(struct kobject *kobj,
                                  struct kobj_attribute *attr,
                                  const char *buf, size_t count)
{
    s32 val;
    if (kstrtos32(buf, 10, &val))
        return -EINVAL;
    gdev->threshold_mC = val;
    return count;
}

static struct kobj_attribute sampling_ms_attr =
    __ATTR(sampling_ms, 0664, sampling_ms_show, sampling_ms_store);
static struct kobj_attribute threshold_mC_attr =
    __ATTR(threshold_mC, 0664, threshold_mC_show, threshold_mC_store);

/* ==================================================== */

static inline bool buf_empty(struct nxp_simtemp_dev *dev)
{
    return dev->head == dev->tail;
}

static void simtemp_work_func(struct work_struct *work)
{
    struct nxp_simtemp_dev *dev = container_of(work, struct nxp_simtemp_dev, work);
    struct simtemp_sample s;
    unsigned long flags;
    ktime_t now = ktime_get();

    s.timestamp_ns = ktime_to_ns(now);
    s.temp_mC = 40000 + (get_random_u32() % 8000) - 4000;
    s.flags = 1;
    if (s.temp_mC > dev->threshold_mC)
        s.flags |= 2;

    spin_lock_irqsave(&dev->lock, flags);
    dev->buffer[dev->head] = s;
    dev->head = (dev->head + 1) % dev->buf_size;
    if (dev->head == dev->tail)
        dev->tail = (dev->tail + 1) % dev->buf_size;
    spin_unlock_irqrestore(&dev->lock, flags);

    wake_up_interruptible(&dev->wq);

    pr_info(DRIVER_NAME ": new sample = %d m°C flags=0x%x (head=%u, tail=%u)\n",
            s.temp_mC, s.flags, dev->head, dev->tail);
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

    gdev->buf_size = 64;
    gdev->buffer = kcalloc(gdev->buf_size, sizeof(struct simtemp_sample), GFP_KERNEL);
    if (!gdev->buffer)
        return -ENOMEM;

    spin_lock_init(&gdev->lock);
    init_waitqueue_head(&gdev->wq);
    INIT_WORK(&gdev->work, simtemp_work_func);

    gdev->sampling_ms = 1000;
    gdev->threshold_mC = 45000;
    gdev->running = true;

    /* Sysfs kobject */
    gdev->kobj = kobject_create_and_add("simtemp", kernel_kobj);
    if (!gdev->kobj)
        return -ENOMEM;

    ret = sysfs_create_file(gdev->kobj, &sampling_ms_attr.attr);
    if (ret)
        return ret;

    ret = sysfs_create_file(gdev->kobj, &threshold_mC_attr.attr);
    if (ret)
        return ret;

    hrtimer_init(&gdev->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    gdev->timer.function = simtemp_timer_cb;
    hrtimer_start(&gdev->timer, ms_to_ktime(gdev->sampling_ms), HRTIMER_MODE_REL);

    gdev->misc.minor = MISC_DYNAMIC_MINOR;
    gdev->misc.name  = DEV_NAME;
    gdev->misc.fops  = &simtemp_fops;
    ret = misc_register(&gdev->misc);
    if (ret)
        return ret;

    pr_info(DRIVER_NAME ": registered /dev/%s\n", DEV_NAME);
    return 0;
}

static void __exit nxp_simtemp_exit(void)
{
    gdev->running = false;
    hrtimer_cancel(&gdev->timer);
    cancel_work_sync(&gdev->work);

    misc_deregister(&gdev->misc);

    if (gdev->kobj)
        kobject_put(gdev->kobj);

    kfree(gdev->buffer);
    kfree(gdev);
    pr_info(DRIVER_NAME ": unregistered\n");
}

module_init(nxp_simtemp_init);
module_exit(nxp_simtemp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Diego Delgado");
MODULE_DESCRIPTION("NXP simulated temperature sensor with sysfs control");
MODULE_VERSION("0.2");
