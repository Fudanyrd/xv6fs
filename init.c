#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <asm/atomic.h>
#include <asm/uaccess.h>
#include "fs.h"

static int xv6fs_init_fs_ctx(struct fs_context *fs_ctx);

struct fs_context_operations xv6fs_context_ops = {
    .parse_param = NULL,
};

static struct file_system_type xv6fs_type = {
    .owner = THIS_MODULE,
    .name = "xv6fs",
    .mount = NULL,
    .kill_sb = NULL,
    .init_fs_context = xv6fs_init_fs_ctx,
	.fs_flags	= FS_REQUIRES_DEV | FS_ALLOW_IDMAP,
};

static int xv6fs_init_fs_ctx(struct fs_context *fs_ctx) {
  return 0;
}

static int __init xv6fs_init(void) {
    return register_filesystem(&xv6fs_type);
}
static void __exit xv6fs_exit(void) {
	unregister_filesystem(&xv6fs_type);
}
module_init(xv6fs_init);
module_exit(xv6fs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Fudanyrd");

