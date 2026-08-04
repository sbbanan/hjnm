#include "linux/sched/signal.h"
#include "linux/types.h"
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/tty.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/sched.h>
#include <linux/pid.h>
#if(LINUX_VERSION_CODE >= KERNEL_VERSION(4,14,83))
#include <linux/sched/mm.h>
#endif
#define ARC_PATH_MAX 256

#include <linux/fs.h>    // For file and d_path
#include <linux/path.h>  // For struct path
#include <linux/dcache.h>// For d_path
#ifndef ARC_PATH_MAX
#define ARC_PATH_MAX PATH_MAX
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
static size_t get_module_base(pid_t pid, char* name)
{
struct vma_iterator vmi;  // 迭代器变量
    struct task_struct* task;
    struct mm_struct* mm;
    struct vm_area_struct *vma;
    uintptr_t count = 0;

    // 1. 通过PID获取进程task_struct
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task)
        return 0;

    // 2. 获取进程内存描述符
    mm = get_task_mm(task);
    if (!mm)
        return 0;

    // 3. 初始化VMA迭代器（传入 &vmi 指针）
    vma_iter_init(&vmi, mm, 0);

    // 4. ✅ 手写循环，彻底绕开有问题的 for_each_vma 宏
    while ((vma = vma_next(&vmi)) != NULL) {
        char buf[ARC_PATH_MAX];
        const char *path_nm = "";

        if (vma->vm_file) {
            file_path(vma->vm_file, buf, ARC_PATH_MAX - 1);
            path_nm = kbasename(buf);  // const 类型匹配

            // 匹配目标模块名
            if (!strcmp(path_nm, name)) {
                count = vma->vm_start;
                break;
            }
        }
    }

    // 5. 释放资源
    mmput(mm);
    return count;
}
#else
uintptr_t get_module_base(pid_t pid, const char *name)
{
    struct task_struct *task;
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    size_t count = 0;
    char buf[ARC_PATH_MAX];
    char *path_nm = "";
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        return 0;
    }

    mm = task->mm;
    if (!mm) {
        return 0;
    }
    for (vma = mm->mmap; vma; vma = vma->vm_next) {
            struct file *file = vma->vm_file;
            if (file) {
                path_nm = d_path(&file->f_path, buf, ARC_PATH_MAX-1);
                if (!strcmp(kbasename(path_nm), name)) {
                    count = vma->vm_start;
                    break;
                }
            }
    }

    mmput(mm);
    return count;
}
#endif
