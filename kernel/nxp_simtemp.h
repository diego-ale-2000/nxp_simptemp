#ifndef NXP_SIMTEMP_H
#define NXP_SIMTEMP_H

#include <linux/types.h>
#include <linux/miscdevice.h>
#include <linux/hrtimer.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/platform_device.h>

/* ================== Defines ================== */
#define DRIVER_NAME "nxp_simtemp"
#define DEV_NAME "simtemp"

/* ================== Data Structures ================== */

/* Structure representing one temperature sample */
struct simtemp_sample {
    __u64 timestamp_ns;  /* Nanosecond timestamp */
    __s32 temp_mC;       /* Temperature in millidegrees Celsius */
    __u32 flags;         /* Bitfield with status flags */
} __attribute__((packed));

/* Main device structure */
struct nxp_simtemp_dev {
    struct miscdevice misc;           // Misc device registration
    struct hrtimer timer;             // High-resolution timer for sampling
    struct work_struct work;          // Workqueue to simulate readings
    spinlock_t lock;                  // Protects buffer access
    wait_queue_head_t wq;             // For blocking reads
    struct simtemp_sample *buffer;    // Circular buffer for samples
    unsigned int buf_size;            // Buffer size
    unsigned int head;                // Write index 
    unsigned int tail;                // Read index
    unsigned int sampling_ms;         // Sampling period
    s32 threshold_mC;                 // Alert threshold
    bool running;                     // Sampling active flag 

    char mode[16];                    // Mode: "normal", "noisy", "ramp"
    struct {
        u32 updates;
        u32 alerts;
        u32 last_error;
    } stats;

    struct kobject *kobj;             // For sysfs exposure
    struct platform_device *pdev;     // Associated platform device
};

#endif /* NXP_SIMTEMP_H */
