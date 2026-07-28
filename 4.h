#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <linux/err.h>
#include <linux/module.h>
#include <asm/ptrace.h>
#include <asm/hw_breakpoint.h>

static pid_t target_pid = 0;
static unsigned long target_va = 0;
static struct perf_event *bp_event = NULL;

static void breakpoint_handler(struct perf_event *event,
                               struct perf_sample_data *data,
                               struct pt_regs *regs)
{
    struct task_struct *tsk = current;

    /* 只处理目标进程且处于用户态 */
    if (tsk->pid != target_pid || !user_mode(regs))
        return;

    if (regs->pc == target_va) {
        regs->regs[0] = 0;          // X0 = 0（返回值）
        regs->pc = regs->regs[30];  // PC = LR，直接返回，跳过原函数
    }
}

int hook_attach(pid_t pid, unsigned long user_va)
{
    struct perf_event_attr attr = {0};
    struct task_struct *tsk;
    struct pid *pid_ptr;

    if (bp_event)
        return -EEXIST;

    pid_ptr = find_vpid(pid);
    if (!pid_ptr)
        return -ESRCH;

    tsk = pid_task(pid_ptr, PIDTYPE_PID);
    if (!tsk || !tsk->mm)
        return -EINVAL;

    target_pid = pid;
    target_va = user_va;

    attr.type = PERF_TYPE_BREAKPOINT;
    attr.size = sizeof(attr);
    attr.bp_type = HW_BREAKPOINT_X;
    attr.bp_addr = user_va;
    attr.bp_len = HW_BREAKPOINT_LEN_4;

    bp_event = perf_event_create_kernel_counter(&attr, -1, tsk,
                                                breakpoint_handler, NULL);
    if (IS_ERR(bp_event)) {
        int err = PTR_ERR(bp_event);
        bp_event = NULL;
        target_pid = 0;
        target_va = 0;
        return err;
    }
    return 0;
}

void hook_detach(void)
{
    if (bp_event) {
        perf_event_release_kernel(bp_event);
        bp_event = NULL;
    }
    target_pid = 0;
    target_va = 0;
}

MODULE_LICENSE("GPL");
