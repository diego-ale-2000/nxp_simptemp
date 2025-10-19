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
#include <linux/device.h>
#include <linux/poll.h>
#include <linux/random.h>
#include <linux/platform_device.h>
#include <linux/of.h>

#define DRIVER_NAME "nxp_simtemp"
#define DEV_NAME "simtemp"

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

    char mode[16];       
    struct {
        u32 updates;
        u32 alerts;
        u32 last_error;
    } stats;

    struct kobject *kobj;
    struct platform_device *pdev;
};


static struct nxp_simtemp_dev *gdev;

/* ================== Sysfs handlers ================== */
static ssize_t sampling_ms_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%u\n", gdev->sampling_ms);
}

static ssize_t sampling_ms_store(struct kobject *kobj, struct kobj_attribute *attr,
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

static ssize_t threshold_mC_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", gdev->threshold_mC);
}

static ssize_t threshold_mC_store(struct kobject *kobj, struct kobj_attribute *attr,
                                  const char *buf, size_t count)
{
    s32 val;
    if (kstrtos32(buf, 10, &val))
        return -EINVAL;
    gdev->threshold_mC = val;
    return count;
}

static ssize_t mode_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%s\n", gdev->mode);
}

static ssize_t mode_store(struct kobject *kobj, struct kobj_attribute *attr,
                          const char *buf, size_t count)
{
    char tmp[16];
    strncpy(tmp, buf, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    if (strncmp(tmp, "normal", 16) != 0 &&
        strncmp(tmp, "noisy", 16) != 0 &&
        strncmp(tmp, "ramp", 16) != 0)
        return -EINVAL;

    strncpy(gdev->mode, tmp, sizeof(gdev->mode));
    return count;
}


static ssize_t stats_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "updates=%u alerts=%u last_error=%u\n",
                   gdev->stats.updates,
                   gdev->stats.alerts,
                   gdev->stats.last_error);
}

static struct kobj_attribute sampling_ms_attr = __ATTR(sampling_ms, 0664, sampling_ms_show, sampling_ms_store);
static struct kobj_attribute threshold_mC_attr = __ATTR(threshold_mC, 0664, threshold_mC_show, threshold_mC_store);
static struct kobj_attribute mode_attr = __ATTR(mode, 0664, mode_show, mode_store);
static struct kobj_attribute stats_attr = __ATTR(stats, 0444, stats_show, NULL);

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

    if (strcmp(dev->mode, "ramp") == 0) {
        static int ramp = 40000;
        ramp += 100; 
        if (ramp > 44000) ramp = 40000;
        s.temp_mC = ramp;
    } else if (strcmp(dev->mode, "noisy") == 0) {
        s.temp_mC = 40000 + (get_random_u32() % 8000) - 4000;
    } else { 
        s.temp_mC = 40000 + (get_random_u32() % 2000) - 1000; // 39–41 °C
    }

    s.flags = 1;
    if (s.temp_mC > dev->threshold_mC)
        s.flags |= 2;

    spin_lock_irqsave(&dev->lock, flags);
    dev->buffer[dev->head] = s;
    dev->head = (dev->head + 1) % dev->buf_size;
    if (dev->head == dev->tail)
        dev->tail = (dev->tail + 1) % dev->buf_size;

    dev->stats.updates++;
    if (s.flags & 2)
        dev->stats.alerts++;
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

/* ================== File Operations ================== */
static int simtemp_open(struct inode *inode, struct file *filp)
{
    filp->private_data = gdev;
    return 0;
}

static ssize_t simtemp_read(struct file *filp, char __user *buf, size_t count, loff_t *off)
{
    struct nxp_simtemp_dev *dev = filp->private_data;
    ssize_t ret = 0;
    unsigned long flags;

    if (count < sizeof(struct simtemp_sample))
        return -EINVAL;

    spin_lock_irqsave(&dev->lock, flags);
    if (buf_empty(dev)) {
        spin_unlock_irqrestore(&dev->lock, flags);
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;
        if (wait_event_interruptible(dev->wq, !buf_empty(dev)))
            return -ERESTARTSYS;
        spin_lock_irqsave(&dev->lock, flags);
    }

    if (copy_to_user(buf, &dev->buffer[dev->tail], sizeof(struct simtemp_sample)))
        ret = -EFAULT;
    else
        ret = sizeof(struct simtemp_sample);

    dev->tail = (dev->tail + 1) % dev->buf_size;
    spin_unlock_irqrestore(&dev->lock, flags);
    return ret;
}

static unsigned int simtemp_poll(struct file *filp, poll_table *wait)
{
    struct nxp_simtemp_dev *dev = filp->private_data;
    unsigned int mask = 0;
    unsigned long flags;

    poll_wait(filp, &dev->wq, wait);

    spin_lock_irqsave(&dev->lock, flags);
    if (!buf_empty(dev))
        mask |= POLLIN | POLLRDNORM;
    if (dev->head != dev->tail) {
        struct simtemp_sample *s = &dev->buffer[dev->tail];
        if (s->flags & 2)
            mask |= POLLPRI;
    }
    spin_unlock_irqrestore(&dev->lock, flags);

    return mask;
}

static const struct file_operations simtemp_fops = {
    .owner = THIS_MODULE,
    .open  = simtemp_open,
    .read  = simtemp_read,
    .poll  = simtemp_poll,
};

/* ================== Platform driver ================== */
static int nxp_simtemp_probe(struct platform_device *pdev)
{
    int ret;

    pr_info(DRIVER_NAME ": probe called\n");

    gdev = kzalloc(sizeof(*gdev), GFP_KERNEL);
    if (!gdev)
        return -ENOMEM;

    gdev->buf_size = 64;
    gdev->buffer = kcalloc(gdev->buf_size, sizeof(struct simtemp_sample), GFP_KERNEL);
    if (!gdev->buffer) {
        kfree(gdev);
        return -ENOMEM;
    }

    spin_lock_init(&gdev->lock);
    init_waitqueue_head(&gdev->wq);
    INIT_WORK(&gdev->work, simtemp_work_func);

    gdev->sampling_ms = 1000;
    gdev->threshold_mC = 45000;
    gdev->running = true;

    strscpy(gdev->mode, "normal", sizeof(gdev->mode));
    gdev->stats.updates = 0;
    gdev->stats.alerts = 0;
    gdev->stats.last_error = 0;

    /* Timer */
    hrtimer_init(&gdev->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    gdev->timer.function = simtemp_timer_cb;
    hrtimer_start(&gdev->timer, ms_to_ktime(gdev->sampling_ms), HRTIMER_MODE_REL);

    /* Misc device */
    gdev->misc.minor = MISC_DYNAMIC_MINOR;
    gdev->misc.name = DEV_NAME;
    gdev->misc.fops = &simtemp_fops;
    ret = misc_register(&gdev->misc);
    if (ret) {
        kfree(gdev->buffer);
        kfree(gdev);
        return ret;
    }

    sysfs_create_file(&gdev->misc.this_device->kobj, &sampling_ms_attr.attr);
    sysfs_create_file(&gdev->misc.this_device->kobj, &threshold_mC_attr.attr);
    sysfs_create_file(&gdev->misc.this_device->kobj, &mode_attr.attr);
    sysfs_create_file(&gdev->misc.this_device->kobj, &stats_attr.attr);

    pr_info(DRIVER_NAME ": /dev/%s ready\n", DEV_NAME);
    return 0;
}

static void nxp_simtemp_remove(struct platform_device *pdev)
{
    struct nxp_simtemp_dev *dev = gdev;

    pr_info(DRIVER_NAME ": remove called\n");

    /* detener timer y work */
    dev->running = false;
    hrtimer_cancel(&dev->timer);
    cancel_work_sync(&dev->work);

    /* eliminar archivos sysfs */
    sysfs_remove_file(&dev->misc.this_device->kobj, &sampling_ms_attr.attr);
    sysfs_remove_file(&dev->misc.this_device->kobj, &threshold_mC_attr.attr);
    sysfs_remove_file(&dev->misc.this_device->kobj, &mode_attr.attr);
    sysfs_remove_file(&dev->misc.this_device->kobj, &stats_attr.attr);

    /* desregistrar misc device */
    misc_deregister(&dev->misc);

    /* liberar buffer y estructura */
    kfree(dev->buffer);
    kfree(dev);

    gdev = NULL;

    pr_info(DRIVER_NAME ": device removed\n");
}


static struct platform_driver nxp_simtemp_driver = {
    .probe  = nxp_simtemp_probe,
    .remove = nxp_simtemp_remove,
    .driver = {
        .name = DRIVER_NAME,
        .owner = THIS_MODULE,
        .of_match_table = NULL,
    },
};

static struct platform_device *nxp_simtemp_pdev;

static int __init nxp_simtemp_init(void)
{
    int ret;

    ret = platform_driver_register(&nxp_simtemp_driver);
    if (ret)
        return ret;

    nxp_simtemp_pdev = platform_device_register_simple("nxp_simtemp", -1, NULL, 0);
    if (IS_ERR(nxp_simtemp_pdev)) {
        platform_driver_unregister(&nxp_simtemp_driver);
        return PTR_ERR(nxp_simtemp_pdev);
    }

    pr_info(DRIVER_NAME ": platform driver registered\n");
    return 0;
}

static void __exit nxp_simtemp_exit(void)
{
    if (nxp_simtemp_pdev)
        platform_device_unregister(nxp_simtemp_pdev);
    platform_driver_unregister(&nxp_simtemp_driver);
    pr_info(DRIVER_NAME ": platform driver unregistered\n");
}

module_init(nxp_simtemp_init);
module_exit(nxp_simtemp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Diego Delgado");
MODULE_DESCRIPTION("NXP simulated temperature sensor as platform driver with sysfs control");
MODULE_VERSION("0.3");
