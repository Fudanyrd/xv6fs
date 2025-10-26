#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/rbtree.h>
#include <linux/uidgid.h>
#include <linux/vfs.h>
#include <asm/atomic.h>
#include <asm/uaccess.h>

#include "fs.h"
#include "fsinfo.h"

/* +-+ locking routine +-+ */

/*
 * Locking conventions https://docs.kernel.org/filesystems/locking.html
 */

static inline struct mutex *xv6_get_lock(struct super_block *sb) {
    struct xv6_fs_info *fi = (struct xv6_fs_info *) sb->s_fs_info;
    return &(fi->build_inode_lock);
}
static inline struct mutex *xv6_balloc_lock(struct super_block *sb) {
    struct xv6_fs_info *fi = (struct xv6_fs_info *) sb->s_fs_info;
    return &(fi->balloc_lock);
}
static inline void xv6_lock(struct super_block *sb) {
    mutex_lock(xv6_get_lock(sb));
}
static inline void xv6_unlock(struct super_block *sb) {
    mutex_unlock(xv6_get_lock(sb));
}

/* Lock the inode table. */
#define xv6_lock_itable(sb) xv6_lock(sb)

/* Unlock the inode table */
#define xv6_unlock_itable(sb) xv6_unlock(sb)

static inline void xv6_ilock_shared(struct inode *ino) {
    down_read(&ino->i_rwsem);
}
static inline void xv6_ilock_exclusive(struct inode *ino) {
    down_write(&ino->i_rwsem);
}
static inline void xv6_iunlock_shared(struct inode *ino) {
    up_read(&ino->i_rwsem);
}
static inline void xv6_iunlock_exclusive(struct inode *ino) {
    up_write(&ino->i_rwsem);
}

/*
 * +-+ balloc.c: allocate/free blocks. 
 * These methods does hold the fs_info's lock.  +-+
 */
/**
 * Search the bitmap for an unused block. If disk full,
 * this will return 0, and set block to 0.
 * @param block[out]: allocated data block.
 * @returns -ERR if error occurred.
 */
static int xv6_balloc(struct super_block *sb, uint *block);
/* call xv6_balloc, and initialize the block to zero. */
static int (*xv6_balloc_zero)(struct super_block *sb, uint *block) 
            = xv6_balloc;
/**
 * Marks `block` as unused. `block` must be a data block.
 * @returns -ERR if error occurred.
 */
static int xv6_bfree(struct super_block *sb, uint block);

/* +-+ inode.c: inode operations. +-+ */
static const struct dentry_operations xv6_dentry_ops;
static const struct inode_operations xv6_inode_ops;
/** 
 * Allocates an inode. Will hold itable lock.
 *
 * @param dino if not NULL, will copy this to disk.
 * @return -ENOSPC if cannot find an unused one.
 */
static int xv6_ialloc(uint *inum, struct super_block *sb,
            const struct dinode *dino);
static int xv6_getattr(struct mnt_idmap *, const struct path *, struct kstat *, 
            u32, unsigned int);
/** 
 * It holds fs lock, allocate an inode, and creates the file.
 */
static int xv6_create(struct mnt_idmap *idmap, struct inode *dir,
            struct dentry *dentry, umode_t mode, bool extc);
/*
 * It ~~holds the lock, and~~ looks up dentry->name in directory.
 * Like ext4_lookup, it does not hold any locks.
 */
static struct dentry *xv6_lookup(struct inode *dir, struct dentry *dentry,
			 unsigned int flags);
/*
 * Compute the hash for the xv6 name corresponding to the dentry.
 * Note: if the name is invalid, we leave the hash code unchanged so
 * that the existing dentry can be used. The xv6 fs routines will
 * return ENOENT or EINVAL as appropriate.
 */
static int xv6_hash(const struct dentry *dentry, struct qstr *qstr);
/*
 * Compare two xv6 names. If either of the names are invalid,
 * we fall back to doing the standard name comparison.
 */
static int xv6_cmp(const struct dentry *dentry,
         unsigned int len, const char *str, const struct qstr *name);
/**
 * This function does not hold lock.
 * @return the initialized inode structure; ERR_PTR(reason) on failure.
 */
static struct inode *xv6_iget(struct super_block *sb, uint inum);
/* Returns 0 if ok; -ERR otherwise. */
static int xv6_init_inode(struct inode *ino, const struct dinode *dino, uint inum);
/* 
 * Sync size, nlink and address of inode to disk inode. 
 * This does not hold lock.
 */
static int xv6_sync_inode(struct inode *ino);
static int xv6_write_inode(struct inode *ino, struct writeback_control *wbc) {
    /* 
     * Assuming that only one copy of inode is present in cache, 
     * It is safe to only hold inode's lock.
     */
    xv6_ilock_shared(ino);
    int ret = xv6_sync_inode(ino);
    xv6_iunlock_shared(ino);
    return ret; 
}
static void xv6_evict_inode(struct inode *ino);
/*
 * Use the cached addresses in ino->i_private to accelerate `xv6_file_block`.
 */
static int xv6_inode_block(struct inode *ino, uint i,
            struct buffer_head **bhptr);
/*
 * Get a block for writing. If a block does not exist, will try to allocate one.
 * If disk full, it returns -ENOSPC, and *bhptr is undefined.
 *
 * If inode is modified, it will properly mark it as dirty.
 */
static int xv6_inode_wblock(struct inode *ino, uint i,
            struct buffer_head **bhptr);
struct dentry *xv6_mkdir (struct mnt_idmap *mmap, struct inode *dir, 
            struct dentry *dentry, umode_t mode);
/* Free all data blocks and indirect block of file. */
static int xv6_inode_clear(struct inode *inode);

/* +-+ dir.c: directory entry operations. These will NOT hold lock. +-+ */
/* Use dir->i_ino to load an on-disk inode. */
static inline int xv6_dget(struct inode *dir, struct dinode *dino);
/**
 * Find a directory entry, and set *inum to its inode number.
 * If no error occurred in reading the dir, but entry is not found,
 * this fill set `inum' to 0.
 *
 * @return 0 on success; `reason' on error.
 */
static int xv6_find_inum(struct inode *dir, struct dentry *entry, uint *inum);
/**
 * Try to allocate an directory entry in [0, dir->size). If no empty
 * entry, *num is set to 0, and returns 0.
 *
 * @return -EEXIT if name already exists
 */
static int xv6_dentry_alloc(struct inode *dir, const char *name, uint *num);

/**
 * Extend directory by one entry, and set *num.
 */
static int xv6_dentry_next(struct inode *dir, uint *num);

static int xv6_dentry_write(struct inode *dir, uint dnum, const char *name, uint inum);
static int xv6_dir_init(struct super_block *sb, uint block, 
            uint inum_parent, uint inum_this);
/*
 * First holds lock, and list the directory.
 */
static int xv6_readdir(struct file *dir, struct dir_context *ctx);

/* 
 * +-+ file.c: file read/write operations. 
 * (directory is organized much like a file) 
 * +-+ 
 */
static const struct file_operations xv6_file_ops;
static const struct file_operations xv6_directory_ops;
/**
 * Sets bhptr to the i'th block of file data. If reaches end,
 * return 0 and set bhptr to NULL.
 *
 * This is deprecated and may be removed in the future.
 */
__attribute__((unused))
static int xv6_file_block(struct super_block *sb, const struct dinode *file,
            uint i, struct buffer_head **bhptr);
/**
 * Get the i'th block of file data. If reaches end, allocate 
 * and attach a block; else it's the same as `xv6_file_block`.
 *
 * If disk is full and cannot allocate, returns -ENOSPC, and
 * bhptr is undefined.
 *
 * @param[out] dirty set this bit to whether dinode is modified.
 * @param[out] bhptr the data block
 * @throw -EROFS if filesystem is read-only.
 */
static int xv6_file_alloc(struct super_block *sb, struct dinode *file,
            uint i, bool *dirty, struct buffer_head **bhptr);
#define xv6_lseek  generic_file_llseek
#define xv6_file_read_iter generic_file_read_iter
static int xv6_update_time(struct inode *a1, int a2) {
    return 0;
}
static ssize_t xv6_file_read(struct file *file, char __user *buf,
            size_t len, loff_t *ppos);
static ssize_t xv6_file_write(struct file *file, const char __user *buf,
            size_t len, loff_t *ppos);
static int xv6_file_sync(struct file *file, loff_t start, loff_t end, int arg4) {
    return xv6_write_inode(file->f_inode, NULL);
}
static int xv6_file_flush(struct file *file, fl_owner_t id) {
    return xv6_write_inode(file->f_inode, NULL);
}

/* +-+ super.c super block operations. +-+ */
enum {
    XV6_UID = 1,
    XV6_GID = 2,
};
static const struct fs_parameter_spec xv6_param_spec[] = {
	fsparam_uid	("uid",		XV6_UID),
	fsparam_gid	("gid",		XV6_GID),
    {},
};

static bool xv6_rb_less(struct rb_node *node1, const struct rb_node *node2);
static struct inode *xv6_alloc_inode(struct super_block *sb);
static void xv6_free_inode(struct inode *inode);
/**
 * First search in inode tree; it it exists, then increment
 * its reference count, and return it; else allocate a new one 
 * via `new_inode', and add it to the tree.
 *
 * @param[out] found only set to true if it is found in the tree,
 *   not allocated and inserted. 
 * @return NULL to indicate -ENOMEM.
 */
static struct inode *xv6_find_inode(struct super_block *sb, uint inum, bool *found);

static int xv6_parse_param(struct fs_context *fc, struct fs_parameter *param);
static int xv6_show_options(struct seq_file *m, struct dentry *root);
static int xv6fs_init_fs_ctx(struct fs_context *fs_ctx);
static int xv6_fill_super(struct super_block *sb, struct fs_context *fc);
static int xv6_get_tree(struct fs_context *fc);
static int xv6_reconfigure(struct fs_context *fc);
static void xv6_free_fc(struct fs_context *fc);
static const struct super_operations xv6_super_ops;
static void xv6_kill_block_super(struct super_block *sb);

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

static int __init xv6fs_init(void) {
    xv6_assert((BSIZE % sizeof(struct dinode)) == 0);
    xv6_assert((BSIZE % sizeof(struct dirent)) == 0);
    return register_filesystem(&xv6fs_type);
}
static void __exit xv6fs_exit(void) {
	unregister_filesystem(&xv6fs_type);
}
module_init(xv6fs_init);
module_exit(xv6fs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Fudanyrd");
MODULE_DESCRIPTION("xv6 simple file system support");
