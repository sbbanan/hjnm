#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include "hw_breakpoint.h"

#ifdef CONFIG_HW_BREAKPOINT_MODE
struct khack_hw_breakpoint {
    struct list_head list;
    struct perf_event *bp_event;
    pid_t pid;
    uintptr_t addr;
};

static LIST_HEAD(g_hw_breakpoints);
static DEFINE_MUTEX(g_hw_bp_mutex);

// 替换cvector：动态指针数组
static HW_BREAKPOINT_HIT_INFO **g_hit_buf = NULL;
static size_t g_hit_len = 0;
static DEFINE_SPINLOCK(g_hit_buffer_lock);

static void breakpoint_handler(struct perf_event *bp, struct perf_sample_data *data, struct pt_regs *regs);

static int add_hw_breakpoint(PHW_BREAKPOINT_CTL ctl)
{
    struct task_struct *task = get_pid_task(find_vpid(ctl->pid), PIDTYPE_PID);
    struct perf_event_attr attr;
    struct perf_event *bp_event;
    struct khack_hw_breakpoint *khack_bp;

    if (!task)
        return -ESRCH;

    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_BREAKPOINT;
    attr.size = sizeof(attr);
    attr.pinned = 1;
    attr.disabled = 1;
    attr.exclude_kernel = attr.exclude_hv = 1;
    attr.bp_len = HW_BREAKPOINT_LEN_8;
    attr.bp_addr = ctl->addr;
    attr.bp_len = ctl->len;

    switch (ctl->type) {
    case HW_BP_TYPE_EXECUTE: attr.bp_type = HW_BREAKPOINT_X; break;
    case HW_BP_TYPE_WRITE:   attr.bp_type = HW_BREAKPOINT_W; break;
    case HW_BP_TYPE_RW:      attr.bp_type = HW_BREAKPOINT_RW; break;
    default: put_task_struct(task); return -EINVAL;
    }

    khack_bp = kmalloc(sizeof(*khack_bp), GFP_KERNEL);
    if (!khack_bp) {
        put_task_struct(task);
        return -ENOMEM;
    }

    bp_event = *register_wide_hw_breakpoint(&attr, breakpoint_handler, task);
    if (IS_ERR_OR_NULL(bp_event)) {
        kfree(khack_bp);
        put_task_struct(task);
        return PTR_ERR(bp_event);
    }

    khack_bp->bp_event = bp_event;
    khack_bp->pid = task->pid;
    khack_bp->addr = ctl->addr;

    mutex_lock(&g_hw_bp_mutex);
    list_add_tail(&khack_bp->list, &g_hw_breakpoints);
    mutex_unlock(&g_hw_bp_mutex);

    put_task_struct(task);
    return 0;
}

static int remove_hw_breakpoint(PHW_BREAKPOINT_CTL ctl)
{
    struct khack_hw_breakpoint *khack_bp, *tmp;
    mutex_lock(&g_hw_bp_mutex);
    list_for_each_entry_safe(khack_bp, tmp, &g_hw_breakpoints, list) {
        if (khack_bp->pid == ctl->pid && khack_bp->addr == ctl->addr) {
            unregister_hw_breakpoint(khack_bp->bp_event);
            list_del(&khack_bp->list);
            kfree(khack_bp);
            mutex_unlock(&g_hw_bp_mutex);
            return 0;
        }
    }
    mutex_unlock(&g_hw_bp_mutex);
    return -ENOENT;
}

static void breakpoint_handler(struct perf_event *bp, struct perf_sample_data *data, struct pt_regs *regs)
{
    unsigned long flags;
    HW_BREAKPOINT_HIT_INFO *hit_info = kmalloc(sizeof(*hit_info), GFP_ATOMIC);
    HW_BREAKPOINT_HIT_INFO **new_buf;

    if (!hit_info)
        return;

    // 可修改寄存器示例
    // regs->ax = 0xDEADBEEF;

    hit_info->pid = current->pid;
    hit_info->timestamp = ktime_get_ns();
    hit_info->addr = instruction_pointer(regs);
    memcpy(&hit_info->regs, regs, sizeof(struct user_pt_regs));

    spin_lock_irqsave(&g_hit_buffer_lock, flags);
    // 扩容数组
    new_buf = krealloc(g_hit_buf, sizeof(HW_BREAKPOINT_HIT_INFO *) * (g_hit_len + 1), GFP_ATOMIC);
    if (new_buf) {
        g_hit_buf = new_buf;
        g_hit_buf[g_hit_len++] = hit_info;
    } else {
        kfree(hit_info);
    }
    spin_unlock_irqrestore(&g_hit_buffer_lock, flags);
}

int handle_hw_breakpoint_control(PHW_BREAKPOINT_CTL ctl)
{
    switch (ctl->action) {
    case HW_BP_ADD:    return add_hw_breakpoint(ctl);
    case HW_BP_REMOVE: return remove_hw_breakpoint(ctl);
    default: return -EINVAL;
    }
}

int handle_hw_breakpoint_get_hits(PHW_BREAKPOINT_GET_HITS_CTL ctl, unsigned long arg)
{
    unsigned long flags;
    size_t hits_to_copy, i;
    int ret = 0;

    spin_lock_irqsave(&g_hit_buffer_lock, flags);
    if (!g_hit_buf) {
        spin_unlock_irqrestore(&g_hit_buffer_lock, flags);
        return -EINVAL;
    }

    hits_to_copy = min(ctl->count, g_hit_len);
    for (i = 0; i < hits_to_copy; i++) {
        if (copy_to_user(&((PHW_BREAKPOINT_HIT_INFO)ctl->buffer)[i], g_hit_buf[i], sizeof(HW_BREAKPOINT_HIT_INFO))) {
            ret = -EFAULT;
            goto out;
        }
    }

    // 释放所有hit对象，清空数组
    for (i = 0; i < g_hit_len; i++)
        kfree(g_hit_buf[i]);
    kfree(g_hit_buf);
    g_hit_buf = NULL;
    g_hit_len = 0;

    ctl->count = hits_to_copy;
    if (copy_to_user((void __user *)arg, ctl, sizeof(*ctl)))
        ret = -EFAULT;
out:
    spin_unlock_irqrestore(&g_hit_buffer_lock, flags);
    return ret;
}

int khack_hw_bp_module_init(void)
{
    g_hit_buf = NULL;
    g_hit_len = 0;
    return 0;
}

void khack_hw_bp_module_exit(void)
{
    struct khack_hw_breakpoint *khack_b