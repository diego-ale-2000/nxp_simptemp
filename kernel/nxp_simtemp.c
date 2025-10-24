#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/of.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/poll.h>

#include "nxp_simtemp.h"

static struct nxp_simtemp_dev *gdev;

/* ============================================================
 *                 SYSFS ATTRIBUTE HANDLERS
 * ============================================================
 * Each handler allows user-space to read or modify driver
 * configuration via /sys/class/misc/simtemp/
 * Attributes:
 *   - sampling_ms
 *   - threshold_mC
 *   - mode
 *   - stats
 * ============================================================ */

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

    /* Accept only valid mode strings */
    if (strncmp(tmp, "normal", 16) != 0 &&
        strncmp(tmp, "noisy", 16) != 0 &&
        strncmp(tmp, "ramp", 16) != 0)
        return -EINVAL;

    strncpy(gdev->mode, tmp, sizeof(gdev->mode));
    return count;
}

/* Read-only system statistics: updates, alerts, and errors */
static ssize_t stats_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "updates=%u alerts=%u last_error=%u\n",
                   gdev->stats.updates,
                   gdev->stats.alerts,
                   gdev->stats.last_error);
}

/* Sysfs attributes registration */
static struct kobj_attribute sampling_ms_attr = __ATTR(sampling_ms, 0664, sampling_ms_show, sampling_ms_store);
static struct kobj_attribute threshold_mC_attr = __ATTR(threshold_mC, 0664, threshold_mC_show, threshold_mC_store);
static struct kobj_attribute mode_attr = __ATTR(mode, 0664, mode_show, mode_store);
static struct kobj_attribute stats_attr = __ATTR(stats, 0444, stats_show, NULL);

/* ============================================================
 *                 SAMPLE GENERATION WORK FUNCTION
 * ============================================================ */

static inline bool buf_empty(struct nxp_simtemp_dev *dev)
{
    return dev->head == dev->tail;
}

/* Called periodically by the hrtimer callback (via workqueue).
 * Simulates a new temperature sample based on current mode. */
static void simtemp_work_func(struct work_struct *work)
{
    struct nxp_simtemp_dev *dev = container_of(work, struct nxp_simtemp_dev, work);
    struct simtemp_sample s;
    unsigned long flags;
    ktime_t now = ktime_get();

    s.timestamp_ns = ktime_to_ns(now);

    /* Generate simulated temperature according to selected mode */
    if (strcmp(dev->mode, "ramp") == 0) {
        static int ramp = RAMP_START_MILLIC;
        ramp += RAMP_STEP_MILLIC;
        if (ramp > RAMP_MAX_MILLIC)
            ramp = RAMP_START_MILLIC;
        s.temp_mC = ramp;

    } else if (strcmp(dev->mode, "noisy") == 0) {
        s.temp_mC = NOISY_MEAN_MILLIC + (get_random_u32() % (2 * NOISY_DELTA_MILLIC)) - NOISY_DELTA_MILLIC;

    } else { /* Normal mode */
        s.temp_mC = NORMAL_MEAN_MILLIC + (get_random_u32() % (2 * NORMAL_DELTA_MILLIC)) - NORMAL_DELTA_MILLIC;
    }

    /* Set flag bits */
    s.flags = 1;
    if (s.temp_mC > dev->threshold_mC)
        s.flags |= 2;

    /* Store sample in circular buffer (protected by spinlock) */
    spin_lock_irqsave(&dev->lock, flags);
    dev->buffer[dev->head] = s;
    dev->head = (dev->head + 1) % dev->buf_size;
    if (dev->head == dev->tail)
        dev->tail = (dev->tail + 1) % dev->buf_size;

    dev->stats.updates++;
    if (s.flags & 2)
        dev->stats.alerts++;
    spin_unlock_irqrestore(&dev->lock, flags);

    /* Wake up any blocking readers */
    wake_up_interruptible(&dev->wq);

    pr_info(DRIVER_NAME ": new sample = %d m°C flags=0x%x (head=%u, tail=%u)\n",
            s.temp_mC, s.flags, dev->head, dev->tail);
}

/* ============================================================
 *                 HIGH-RESOLUTION TIMER CALLBACK
 * ============================================================
 * Triggers the sample generation work periodically.
 * ============================================================ */
static enum hrtimer_restart simtemp_timer_cb(struct hrtimer *t)
{
    struct nxp_simtemp_dev *dev = container_of(t, struct nxp_simtemp_dev, timer);

    if (!dev->running)
        return HRTIMER_NORESTART;

    schedule_work(&dev->work);
    hrtimer_forward_now(&dev->timer, ms_to_ktime(dev->sampling_ms));
    return HRTIMER_RESTART;
}

/* ============================================================
 *                 CHARACTER DEVICE INTERFACE
 * ============================================================
 * Exposes /dev/simtemp for user-space reads and polling.
 * ============================================================ */

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

    /* Wait for data if buffer is empty */
    spin_lock_irqsave(&dev->lock, flags);
    if (buf_empty(dev)) {
        spin_unlock_irqrestore(&dev->lock, flags);
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;
        if (wait_event_interruptible(dev->wq, !buf_empty(dev)))
            return -ERESTARTSYS;
        spin_lock_irqsave(&dev->lock, flags);
    }

    /* Copy sample to user space */
    if (copy_to_user(buf, &dev->buffer[dev->tail], sizeof(struct simtemp_sample)))
        ret = -EFAULT;
    else
        ret = sizeof(struct simtemp_sample);

    /* Update tail pointer */
    dev->tail = (dev->tail + 1) % dev->buf_size;
    spin_unlock_irqrestore(&dev->lock, flags);
    return ret;
}

/* Support poll() and select() system calls for async user-space I/O */
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

/* ============================================================
 *                 PLATFORM DRIVER IMPLEMENTATION
 * ============================================================ */

static int nxp_simtemp_probe(struct platform_device *pdev)
{
    int ret;

    pr_info(DRIVER_NAME ": probe called\n");

    /* Allocate and initialize driver context */
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

    /* Initialize stats */
    gdev->stats.updates = 0;
    gdev->stats.alerts = 0;
    gdev->stats.last_error = 0;

    /* Configure and start timer */
    hrtimer_init(&gdev->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    gdev->timer.function = simtemp_timer_cb;
    hrtimer_start(&gdev->timer, ms_to_ktime(gdev->sampling_ms), HRTIMER_MODE_REL);

    /* Register misc device under /dev/simtemp */
    gdev->misc.minor = MISC_DYNAMIC_MINOR;
    gdev->misc.name = DEV_NAME;
    gdev->misc.fops = &simtemp_fops;
    ret = misc_register(&gdev->misc);
    if (ret) {
        kfree(gdev->buffer);
        kfree(gdev);
        return ret;
    }

    /* Create sysfs attributes */
    ret = sysfs_create_file(&gdev->misc.this_device->kobj, &sampling_ms_attr.attr);
    if (ret)
        dev_warn(&pdev->dev, "failed to create sampling_ms sysfs file\n");

    ret = sysfs_create_file(&gdev->misc.this_device->kobj, &threshold_mC_attr.attr);
    if (ret)
        dev_warn(&pdev->dev, "failed to create threshold_mC sysfs file\n");

    ret = sysfs_create_file(&gdev->misc.this_device->kobj, &mode_attr.attr);
    if (ret)
        dev_warn(&pdev->dev, "failed to create mode sysfs file\n");

    ret = sysfs_create_file(&gdev->misc.this_device->kobj, &stats_attr.attr);
    if (ret)
        dev_warn(&pdev->dev, "failed to create stats sysfs file\n");

    pr_info(DRIVER_NAME ": /dev/%s ready\n", DEV_NAME);
    return 0;
}

/* Cleanup on driver removal */
static void nxp_simtemp_remove(struct platform_device *pdev)
{
    struct nxp_simtemp_dev *dev = gdev;

    pr_info(DRIVER_NAME ": remove called\n");

    dev->running = false;
    hrtimer_cancel(&dev->timer);
    cancel_work_sync(&dev->work);

    /* Remove sysfs attributes */
    sysfs_remove_file(&dev->misc.this_device->kobj, &sampling_ms_attr.attr);
    sysfs_remove_file(&dev->misc.this_device->kobj, &threshold_mC_attr.attr);
    sysfs_remove_file(&dev->misc.this_device->kobj, &mode_attr.attr);
    sysfs_remove_file(&dev->misc.this_device->kobj, &stats_attr.attr);

    misc_deregister(&dev->misc);

    kfree(dev->buffer);
    kfree(dev);
    gdev = NULL;

    pr_info(DRIVER_NAME ": device removed\n");
}

/* ============================================================
 *                 DRIVER REGISTRATION
 * ============================================================ */

static struct platform_driver nxp_simtemp_driver = {
    .probe  = nxp_simtemp_probe,
    .remove = nxp_simtemp_remove,
    .driver = {
        .name = DRIVER_NAME,
        .owner = THIS_MODULE,
        .of_match_table = NULL,  /* No Device Tree binding used */
    },
};

static struct platform_device *nxp_simtemp_pdev;

static int __init nxp_simtemp_init(void)
{
    int ret;

    /* Register driver and create a synthetic platform device */
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

/* ============================================================
 *                 MODULE METADATA
 * ============================================================ */

module_init(nxp_simtemp_init);
module_exit(nxp_simtemp_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Diego Delgado");
MODULE_DESCRIPTION("NXP simulated temperature sensor as platform driver with sysfs control");
MODULE_VERSION("0.3");
