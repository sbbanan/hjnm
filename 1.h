#include <linux/slab.h>
#include <linux/random.h>

#define DEVICE_NAME "hqdw" //当前驱动DEV文件
typedef struct _COPY_MEMORY {
    pid_t pid;
    uintptr_t addr;
    void* buffer;
    size_t size;
} COPY_MEMORY, *PCOPY_MEMORY;

typedef struct _MODULE_BASE {
    pid_t pid;
    char* name;
    uintptr_t base;
} MODULE_BASE, *PMODULE_BASE;

enum OPERATIONS {
    OP_INIT_KEY = 0x800,
    OP_READ_MEM = 0x801,
    OP_WRITE_MEM = 0x802,
    OP_MODULE_BASE = 0x803,
    OP_HW_BREAKPOINT_CTL = 0x804,
     OP_HW_BREAKPOINT_GET_HITS = 0x805
};


int dispatch_open(struct inode *node, struct file *file);
int dispatch_close(struct inode *node, struct file *file);