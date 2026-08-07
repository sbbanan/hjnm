#include "drv_bp.h"
#include <linux/slab.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/list.h>

LIST_HEAD(g_bp_list);
DEFINE_SPINLOCK(g_bp_lock);
HW_BREAKPOINT_HIT_INFO g_hit_buf[BP_HIT_MAX_CNT];
atomic_t g_hit_idx = ATOMIC_INIT(0);

// 类型转换：用户枚举 -> perf标准掩码
static int bp_type_to_mask(enum HW_BP_TYPE t)
{
    switch(t) {
    case HW_BP_TYPE_EXECUTE: return HW_BREAKPOINT_X;
    case HW_BP_TYPE_WRITE:   return HW_BREAKPOINT_W;
    case HW_BP_TYPE_RW:      return HW_BREAKPOINT_RW;
    default: return -EINVAL;
    }
}

// 断点溢出回调：命中时保存寄存器、时间、进程信息
void hw_bp_overflow_cb(struct perf_event *event, struct perf_sample_data *data, struct pt_regs *regs)
{
    struct hw_bp_node *node = event->private_data;
    int idx = atomic_fetch_inc(&g_hit_idx) % BP_HIT_MAX_CNT;
    HW_BREAKPOINT_HIT_INFO *info = &g_hit_buf[idx];

    info->pid = task_pid_nr(current);
    info->timestamp = ktime_to_ms(ktime_get());
    info->addr = node->addr;
    // 拷贝用户态寄存器快照
    memcpy(&info->regs, regs, sizeof(struct user_pt_regs));
}

// 根据pid+addr查找已存在断点节点
static struct hw_bp_node *find_bp_node(pid_t pid, uintptr_t addr)
{
    struct hw_bp_node *cur;
    list_for_each_entry(cur, &g_bp_list, list) {
        if (cur->pid == pid && cur->addr == addr)
            return cur;
    }
    return NULL;
}

// 添加硬件断点
static int add_hw_breakpoint(HW_BREAKPOINT_CTL *ctl)
{
    struct task_struct *target_task;
    struct perf_event_attr attr;
    struct hw_bp_node *node;
    int mask, ret;

    mask = bp_type_to_mask(ctl->type);
    if (mask < 0 || ctl->len <=0 || ctl->len >8)
        return -EINVAL;

    // 检查进程是否存在
    target_task = pid_task(find_vpid(ctl->pid), PIDTYPE_PID);
    if (!target_task)
        return -ESRCH;

    spin_lock(&g_bp_lock);
    // 禁止重复添加同地址断点
    if (find_bp_node(ctl->pid, ctl->addr)) {
        spin_unlock(&g_bp_lock);
        return -EEXIST;
    }
    spin_unlock(&g_bp_lock);

    // 初始化perf断点属性
    memset(&attr, 0, sizeof(attr));
    hw_breakpoint_init(&attr);
    attr.bp_addr = ctl->addr;
    attr.bp_len = ctl->len;
    attr.bp_type = mask;
    attr.sample_period = 1; // 每命中一次触发回调

    node = kzalloc(sizeof(*node), GFP_KERNEL);
    if (!node)
        return -ENOMEM;

    // 创建perf事件，绑定目标进程
    node->event = perf_event_create_kernel_counter(
        &attr,
        -1,                     // 所有CPU
        target_task,            // 绑定目标进程
        hw_bp_overflow_cb,      // 触发回调
        node                    // 私有数据
    );
    if (IS_ERR(node->event)) {
        ret = PTR_ERR(node->event);
        kfree(node);
        return ret;
    }

    node->pid = ctl->pid;
    node->addr = ctl->addr;
    node->len = ctl->len;
    node->bp_mask = mask;

    spin_lock(&g_bp_lock);
    list_add_tail(&node->list, &g_bp_list);
    spin_unlock(&g_bp_lock);
    return 0;
}

// 删除指定断点
static int remove_hw_breakpoint(HW_BREAKPOINT_CTL *ctl)
{
    struct hw_bp_node *node;
    spin_lock(&g_bp_lock);
    node = find_bp_node(ctl->pid, ctl->addr);
    if (!node) {
        spin_unlock(&g_bp_lock);
        return -ENOENT;
    }
    list_del(&node->list);
    spin_unlock(&g_bp_lock);

    // 释放perf断点资源
    perf_event_release_kernel(node->event);
    kfree(node);
    return 0;
}

// IOCTL：OP_HW_BREAKPOINT_CTL 统一增删入口
int handle_hw_breakpoint_control(HW_BREAKPOINT_CTL *usr_ctl)
{
    switch(usr_ctl->action) {
    case HW_BP_ADD:
        return add_hw_breakpoint(usr_ctl);
    case HW_BP_REMOVE:
        return remove_hw_breakpoint(usr_ctl);
    default:
        return -EINVAL;
    }
}

// IOCTL：OP_HW_BREAKPOINT_GET_HITS 拷贝命中记录到用户层
int handle_hw_breakpoint_get_hits(HW_BREAKPOINT_GET_HITS_CTL __user *usr_arg)
{
    HW_BREAKPOINT_GET_HITS_CTL k_ctl;
    size_t copy_cnt, i;
    int cur_idx;
    // 拷贝用户参数
    if (copy_from_user(&k_ctl, usr_arg, sizeof(k_ctl)))
        return -EFAULT;
    if (!k_ctl.buffer || k_ctl.count ==0)
        return -EINVAL;

    cur_idx = atomic_read(&g_hit_idx);
    copy_cnt = cur_idx > k_ctl.count ? k_ctl.count : cur_idx;

    // 批量拷贝命中记录到用户缓冲区
    for(i=0; i<copy_cnt; i++) {
        HW_BREAKPOINT_HIT_INFO *src = &g_hit_buf[i];
        if (copy_to_user(&k_ctl.buffer[i], src, sizeof(*src)))
            return -EFAULT;
    }
    // 重置命中计数，读完清空缓存
    atomic_set(&g_hit_idx, 0);
    // 回写实际读取条数给用户
    k_ctl.count = copy_cnt;
    if (copy_to_user(usr_arg, &k_ctl, sizeof(k_ctl)))
        return -EFAULT;
    return 0;
}

// 模块卸载：销毁全部断点
void hw_bp_clean_all(void)
{
    struct hw_bp_node *node, *tmp;
    spin_lock(&g_bp_lock);
    list_for_each_entry_safe(node, tmp, &g_bp_list, list) {
        list_del(&node->list);
        spin_unlock(&g_bp_lock);
        perf_event_release_kernel(node->event);
        kfree(node);
        spin_lock(&g_bp_lock);
    }
    spin_unlock(&g_bp_lock);
    atomic_set(&g_hit_idx, 0);
}
