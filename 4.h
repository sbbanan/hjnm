#ifndef DRV_BP_H
#define DRV_BP_H
#include <linux/types.h>
#include <linux/sched.h>
#include <asm/ptrace.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>

// 和用户层完全对齐，防止结构体错位
enum HW_BP_ACTION {
    HW_BP_ADD,
    HW_BP_REMOVE
};
enum HW_BP_TYPE {
    HW_BP_TYPE_DUMMY = 0,
    HW_BP_TYPE_EXECUTE,
    HW_BP_TYPE_WRITE,
    HW_BP_TYPE_RW
};
typedef struct {
    enum HW_BP_ACTION action;
    pid_t pid;
    uintptr_t addr;
    int len;
    enum HW_BP_TYPE type;
} HW_BREAKPOINT_CTL;
typedef struct {
    pid_t pid;
    u64 timestamp;
    uintptr_t addr;
    struct user_pt_regs regs;
} HW_BREAKPOINT_HIT_INFO;
typedef struct {
    size_t count;
    HW_BREAKPOINT_HIT_INFO __user *buffer;
} HW_BREAKPOINT_GET_HITS_CTL;
// IOCTL cmd 与用户侧一一对应
#define OP_HW_BREAKPOINT_CTL        0x804
#define OP_HW_BREAKPOINT_GET_HITS   0x805

// 驱动内部断点管理节点
struct hw_bp_node {
    struct list_head list;
    pid_t pid;
    uintptr_t addr;
    int len;
    int bp_mask;    // perf断点掩码 HW_BREAKPOINT_X / HW_BREAKPOINT_W / HW_BREAKPOINT_RW
    struct perf_event *event;
};
// 最大缓存命中条数，可按需修改
#define BP_HIT_MAX_CNT 128
extern struct list_head g_bp_list;
extern spinlock_t g_bp_lock;
extern HW_BREAKPOINT_HIT_INFO g_hit_buf[BP_HIT_MAX_CNT];
extern atomic_t g_hit_idx;

// 对外IOCTL处理函数
int handle_hw_breakpoint_control(HW_BREAKPOINT_CTL *usr_ctl);
int handle_hw_breakpoint_get_hits(HW_BREAKPOINT_GET_HITS_CTL __user *usr_arg);
// 模块卸载清理全部断点
void hw_bp_clean_all(void);
// 断点触发回调
void hw_bp_overflow_cb(struct perf_event *event, struct perf_sample_data *data, struct pt_regs *regs);
#endif
