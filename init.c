#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/mpage.h>
#include <linux/rbtree.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/uidgid.h>
#include <linux/vfs.h>
#include <asm/atomic.h>
#include <asm/uaccess.h>

#include "fs.h"
#include "fsinfo.h"

#include "xv6.h"
#include "check.h"

EXPORT_SYMBOL_GPL(xv6_docheck);

static struct kmem_cache *xv6_inode_cachep;

static const struct fs_context_operations xv6fs_context_ops = {
    .parse_param = xv6_parse_param,
    .get_tree = xv6_get_tree,
    .reconfigure = xv6_reconfigure,
    .free = xv6_free_fc,
};

static struct file_system_type xv6fs_type = {
    .owner = THIS_MODULE,
    .name = "xv6fs",
    .mount = NULL,
    .kill_sb = xv6_kill_block_super,
    .init_fs_context = xv6fs_init_fs_ctx,
	.fs_flags	= FS_REQUIRES_DEV | FS_ALLOW_IDMAP,
    .parameters = xv6_param_spec,
};

#include "balloc.c"
#include "inode.c"
#include "dir.c"
#include "file.c"
#include "super.c"

static void checker_printk(const char *fmt, ...) {
    va_list vl;
    va_start(vl, fmt);
    vprintk(fmt, vl);
    va_end(vl);
}

static void *checker_bread(void *privat, uint block) {
    struct super_block *sb = privat;
    return sb_bread(sb, block);
}

static void *checker_data(void *buffer) {
    struct buffer_head *bh = buffer;
    return bh->b_data;
}

static void checker_bfree(void *buffer) {
    struct buffer_head *bh = buffer;
    brelse(bh);
}

static void xv6_init_once(void *pt) {
    struct xv6_inode *xi = pt;
    inode_init_once(&xi->inode);
    xi->refcount = 1;
}

static int __init xv6fs_init(void) {
    xv6_assert((BSIZE % sizeof(struct dinode)) == 0);
    xv6_assert((BSIZE % sizeof(struct dirent)) == 0);
	xv6_inode_cachep = kmem_cache_create("xv6_cache",
				sizeof(struct xv6_inode),
				0, SLAB_RECLAIM_ACCOUNT,
				xv6_init_once);
	if (xv6_inode_cachep == NULL)
		return -ENOMEM;
    return register_filesystem(&xv6fs_type);
}
static void __exit xv6fs_exit(void) {
	kmem_cache_destroy(xv6_inode_cachep);
	unregister_filesystem(&xv6fs_type);
}
module_init(xv6fs_init);
module_exit(xv6fs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Fudanyrd");
MODULE_DESCRIPTION("xv6 simple file system support");
