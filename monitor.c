/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 *
 * Provided boilerplate:
 *   - device registration and teardown
 *   - timer setup
 *   - RSS helper
 *   - soft-limit and hard-limit event helpers
 *   - ioctl dispatch shell
 *
 * YOUR WORK: Fill in all sections marked // TODO.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ==============================================================
 * TODO 1: Define your linked-list node struct.
 *
 * Requirements:
 *   - track PID, container ID, soft limit, and hard limit
 *   - remember whether the soft-limit warning was already emitted
 *   - include `struct list_head` linkage
 * ============================================================== */

/*
 * monitor_entry - an element of the monitored process list
 *
 * This structure stores per-process state for memory monitoring.  Each
 * entry tracks the host PID of the process, an ASCII container ID
 * string (truncated to MONITOR_NAME_LEN), soft and hard memory limits
 * expressed in bytes, and a flag indicating whether a soft-limit
 * warning has already been logged.  The `list` member links the
 * entry into the global monitored list.  Using a separate structure
 * instead of embedding fields directly in the kernel module state
 * allows us to allocate and free entries dynamically as processes
 * register and unregister.
 */
struct monitor_entry {
    pid_t pid;
    char container_id[MONITOR_NAME_LEN];
    unsigned long soft_limit;
    unsigned long hard_limit;
    bool soft_emitted;
    struct list_head list;
};


/* ==============================================================
 * TODO 2: Declare the global monitored list and a lock.
 *
 * Requirements:
 *   - shared across ioctl and timer code paths
 *   - protect insert, remove, and iteration safely
 *
 * You may choose either a mutex or a spinlock, but your README must
 * justify the choice in terms of the code paths you implemented.
 * ============================================================== */

/*
 * Global list of monitored processes.  All access to this list must
 * be synchronized because it is manipulated from both process context
 * (ioctl handlers) and timer callback context.  Since the timer
 * callback runs in soft interrupt context and cannot sleep, we use a
 * spinlock to protect the list.  The spinlock is held only for the
 * short critical sections where the list is traversed or modified.
 */
static LIST_HEAD(monitored_list);
static DEFINE_SPINLOCK(monitored_lock);


/* --- Provided: internal device / timer state --- */
static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

/* ---------------------------------------------------------------
 * Provided: RSS Helper
 *
 * Returns the Resident Set Size in bytes for the given PID,
 * or -1 if the task no longer exists.
 * --------------------------------------------------------------- */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ---------------------------------------------------------------
 * Provided: soft-limit helper
 *
 * Log a warning when a process exceeds the soft limit.
 * --------------------------------------------------------------- */
static void log_soft_limit_event(const char *container_id,
                                 pid_t pid,
                                 unsigned long limit_bytes,
                                 long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Provided: hard-limit helper
 *
 * Kill a process when it exceeds the hard limit.
 * --------------------------------------------------------------- */
static void kill_process(const char *container_id,
                         pid_t pid,
                         unsigned long limit_bytes,
                         long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Timer Callback - fires every CHECK_INTERVAL_SEC seconds.
 * --------------------------------------------------------------- */
static void timer_callback(struct timer_list *t)
{
    /* ==============================================================
     * TODO 3: Implement periodic monitoring.
     *
     * Requirements:
     *   - iterate through tracked entries safely
     *   - remove entries for exited processes
     *   - emit soft-limit warning once per entry
     *   - enforce hard limit and then remove the entry
     *   - avoid use-after-free while deleting during iteration
     * ============================================================== */

    /*
     * Iterate over each monitored entry and evaluate its current RSS
     * consumption against the configured soft and hard limits.  The
     * iteration uses the list_for_each_entry_safe macro so that we can
     * remove entries on the fly without corrupting the list.  All
     * manipulations are protected by the monitored_lock spinlock to
     * prevent concurrent modifications from the ioctl handler.
     */
    struct monitor_entry *entry, *tmp;

    spin_lock(&monitored_lock);
    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        long rss_bytes;

        /* Query the current RSS.  A negative return value indicates
         * that the process has already exited; remove the entry.
         */
        rss_bytes = get_rss_bytes(entry->pid);
        if (rss_bytes < 0) {
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        /* Soft limit check: emit a warning once when the RSS first
         * exceeds the soft limit.  Do not emit the warning again for
         * the same process.  Soft limit of zero means no soft limit.
         */
        if (!entry->soft_emitted && entry->soft_limit > 0 &&
            (unsigned long)rss_bytes > entry->soft_limit) {
            log_soft_limit_event(entry->container_id, entry->pid,
                                 entry->soft_limit, rss_bytes);
            entry->soft_emitted = true;
        }

        /* Hard limit check: if RSS exceeds the hard limit (non-zero),
         * kill the process and remove the entry.  After sending the
         * signal we immediately remove the entry from the list and
         * free it.  This avoids repeatedly sending SIGKILL on
         * subsequent timer ticks.  Hard limit of zero means no limit.
         */
        if (entry->hard_limit > 0 && (unsigned long)rss_bytes > entry->hard_limit) {
            /* It is safe to call kill_process while holding the
             * spinlock because send_sig does not sleep.  Removing the
             * entry here prevents use-after-free in future iterations.
             */
            kill_process(entry->container_id, entry->pid,
                         entry->hard_limit, rss_bytes);
            list_del(&entry->list);
            kfree(entry);
            continue;
        }
    }
    spin_unlock(&monitored_lock);

    /* Re-arm the timer for the next interval. */
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ---------------------------------------------------------------
 * IOCTL Handler
 *
 * Supported operations:
 *   - register a PID with soft + hard limits
 *   - unregister a PID when the runtime no longer needs tracking
 * --------------------------------------------------------------- */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {
        printk(KERN_INFO
               "[container_monitor] Registering container=%s pid=%d soft=%lu hard=%lu\n",
               req.container_id, req.pid, req.soft_limit_bytes, req.hard_limit_bytes);

        /*
         * Allocate and initialize a new monitor entry from the
         * user-provided request.  Validate that the requested limits
         * are sensible (soft cannot exceed hard when both are non-zero)
         * and that memory allocation succeeds.  The container ID is
         * truncated to MONITOR_NAME_LEN-1 to guarantee NUL-termination.
         */
        struct monitor_entry *new_ent;

        if (req.soft_limit_bytes > 0 &&
            req.hard_limit_bytes > 0 &&
            req.soft_limit_bytes > req.hard_limit_bytes) {
            return -EINVAL;
        }

        new_ent = kzalloc(sizeof(*new_ent), GFP_KERNEL);
        if (!new_ent)
            return -ENOMEM;

        new_ent->pid = req.pid;
        /* Copy and ensure NUL termination */
        strscpy(new_ent->container_id, req.container_id,
        sizeof(new_ent->container_id));
        
        new_ent->soft_limit = req.soft_limit_bytes;
        new_ent->hard_limit = req.hard_limit_bytes;
        new_ent->soft_emitted = false;
        INIT_LIST_HEAD(&new_ent->list);

        /* Insert into the monitored list under the spinlock */
        spin_lock(&monitored_lock);
        list_add_tail(&new_ent->list, &monitored_list);
        spin_unlock(&monitored_lock);

        return 0;
    }

    printk(KERN_INFO
           "[container_monitor] Unregister request container=%s pid=%d\n",
           req.container_id, req.pid);

    /*
     * Search for an existing entry matching both the container ID and
     * PID (we match on both to avoid unintended removals when PIDs
     * are reused).  If found, remove it from the list and free it.
     * Return 0 on success, -ENOENT when no matching entry exists.
     */
    {
        struct monitor_entry *entry, *tmp;
        int removed = 0;

        spin_lock(&monitored_lock);
        list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
            if (entry->pid == req.pid &&
                strncmp(entry->container_id, req.container_id,
                        sizeof(entry->container_id)) == 0) {
                list_del(&entry->list);
                kfree(entry);
                removed = 1;
                break;
            }
        }
        spin_unlock(&monitored_lock);

        return removed ? 0 : -ENOENT;
    }
}

/* --- Provided: file operations --- */
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* --- Provided: Module Init --- */
static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Module loaded. Device: /dev/%s\n", DEVICE_NAME);
    return 0;
}

/* --- Provided: Module Exit --- */
static void __exit monitor_exit(void)
{
    del_timer_sync(&monitor_timer);


    /*
     * Clean up any monitored entries remaining on the list.  Iterate
     * through the list and free each node.  The spinlock ensures
     * exclusive access while we tear down the list.  Because kfree
     * does not sleep, it is safe to call with the spinlock held.
     */
    spin_lock(&monitored_lock);
    while (!list_empty(&monitored_list)) {
        struct monitor_entry *entry;
        entry = list_first_entry(&monitored_list, struct monitor_entry, list);
        list_del(&entry->list);
        kfree(entry);
    }
    spin_unlock(&monitored_lock);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
