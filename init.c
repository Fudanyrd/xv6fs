#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/uidgid.h>
#include <linux/vfs.h>
#include <asm/atomic.h>
#include <asm/uaccess.h>

#include "fs.h"
#include "fsinfo.h"

/* locking routine */

static inline struct mutex *xv6_get_lock(struct super_block *sb) {
    struct xv6_fs_info *fi = (struct xv6_fs_info *) sb->s_fs_info;
    return &(fi->build_inode_lock);
}
static inline void xv6_lock(struct super_block *sb) {
    mutex_lock(xv6_get_lock(sb));
}
static inline void xv6_unlock(struct super_block *sb) {
    mutex_unlock(xv6_get_lock(sb));
}

/*
 * +-+ balloc.c: allocate/free blocks. 
 * These methods does NOT hold the fs lock.  +-+
 */
/**
 * Search the bitmap for an unused block.
 * @returns 0 if disk full.
 */
static uint xv6_balloc(struct super_block *sb);
/*
 * Marks `block` as unused. `block` must be a data block.
 */
static void xv6_bfree(struct super_block *sb, uint block);

/* +-+ inode.c: inode operations. +-+ */
static const struct dentry_operations xv6_dentry_ops;
static const struct inode_operations xv6_inode_ops;
/** 
 * It holds fs lock, allocate an inode, and creates the file.
 */
static int xv6_create(struct mnt_idmap *idmap, struct inode *dir,
            struct dentry *dentry, umode_t mode, bool extc);
/*
 * It holds the lock, and look up dentry->name in directory.
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
static int xv6_init_inode(struct inode *ino, const struct dinode *dino);

/* +-+ dir.c: directory entry operations. These will NOT hold lock. +-+ */
/**
 * Find a directory entry, and set *inum to its inode number.
 * If no error occurred in reading the dir, but entry is not found,
 * this fill set `inum' to 0.
 *
 * @return 0 on success; `reason' on error.
 */
static int xv6_find_inum(struct inode *dir, struct dentry *entry, uint *inum);

/* 
 * +-+ file.c: file read/write operations. 
 * (directory is organized much like a file) 
 * +-+ 
 */


#include "balloc.c"
#include "inode.c"
#include "dir.c"
#include "file.c"

enum {
    XV6_UID = 1,
    XV6_GID = 2,
};
static const struct fs_parameter_spec xv6_param_spec[] = {
	fsparam_uid	("uid",		XV6_UID),
	fsparam_gid	("gid",		XV6_GID),
    {},
};

static int xv6_parse_param(struct fs_context *fc, struct fs_parameter *param);
static int xv6_show_options(struct seq_file *m, struct dentry *root);
static int xv6fs_init_fs_ctx(struct fs_context *fs_ctx);
static int xv6_fill_super(struct super_block *sb, struct fs_context *fc);
static int xv6_get_tree(struct fs_context *fc) {
    return get_tree_bdev(fc, xv6_fill_super);
}
static int xv6_reconfigure(struct fs_context *fc);
static void xv6_free_fc(struct fs_context *fc) {
    if (fc->fs_private) {
        kfree(fc->fs_private);
    }
}

static struct fs_context_operations xv6fs_context_ops = {
    .parse_param = xv6_parse_param,
    .get_tree = xv6_get_tree,
    .reconfigure = xv6_reconfigure,
    .free = xv6_free_fc,
};

static struct file_system_type xv6fs_type = {
    .owner = THIS_MODULE,
    .name = "xv6fs",
    .mount = NULL,
    .kill_sb = kill_block_super,
    .init_fs_context = xv6fs_init_fs_ctx,
	.fs_flags	= FS_REQUIRES_DEV | FS_ALLOW_IDMAP,
    .parameters = xv6_param_spec,
};

static struct super_operations xv6_super_ops = {
    .show_options = xv6_show_options,
};

static int xv6fs_init_fs_ctx(struct fs_context *fc) {
    fc->ops = &xv6fs_context_ops;
    void *buf = kzalloc(sizeof(struct xv6_mount_options), GFP_KERNEL);
    if (!buf) {
        return -ENOMEM;
    }
    fc->fs_private = buf;
    return 0;
}

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


static int xv6_fill_super(struct super_block *sb, struct fs_context *fc) {

	struct buffer_head *bh = NULL;
    struct xv6_fs_info *fsinfo = NULL;
    long error;
    struct inode *root_dir = NULL;

    fsinfo = kzalloc(sizeof(struct xv6_fs_info), GFP_KERNEL);
    if (!fsinfo) {
        return -ENOMEM;
    }

    sb_set_blocksize(sb, BSIZE);
	sb->s_fs_info = fsinfo;
	sb->s_flags |= SB_NODIRATIME;
	sb->s_magic = FSMAGIC;
    sb->s_op = &xv6_super_ops;
	sb->s_export_op = NULL /* FIXME */;
	sb->s_time_gran = 1;
    sb->s_time_min = 0;
    sb->s_time_max = (time64_t) (9223372036854775807L);

	set_default_d_op(sb, NULL /* FIXME */);
    bh = sb_bread(sb, 0);
    if (bh == NULL) {
        /* Unable to read super block. */
        error = -EIO;
        goto out_fail;
    }
    const struct superblock *xv6_sb = (const struct superblock *) bh->b_data;
    if (__le32_to_cpu(xv6_sb->magic) != FSMAGIC) {
        /* Not a xv6 filesystem. */
        error = -EINVAL;
        goto out_fail;
    }
    fsinfo->size = __le32_to_cpu(xv6_sb->size);
    fsinfo->nblocks = __le32_to_cpu(xv6_sb->nblocks);
    fsinfo->ninodes = __le32_to_cpu(xv6_sb->ninodes);
    fsinfo->nlog = __le32_to_cpu(xv6_sb->nlog);
    fsinfo->logstart = __le32_to_cpu(xv6_sb->logstart);
    fsinfo->inodestart = __le32_to_cpu(xv6_sb->inodestart);
    fsinfo->bmapstart = __le32_to_cpu(xv6_sb->bmapstart);
    brelse(bh); bh = NULL;

    struct dirent dummy;
    const typeof(dummy.inum) ninodes_max = (typeof(dummy.inum)) (-1);
    if (fsinfo->ninodes > ninodes_max) {
        xv6_warn("Too many inodes (max %u supported)", ninodes_max);
        fsinfo->ninodes = ninodes_max;
        /* FIXME: also update super block on disk. */
    }
    error = -EINVAL;
    fsinfo->ninode_blocks = INODE_BLOCKS(fsinfo->ninodes);
    fsinfo->nbmap_blocks = BITMAP_BLOCKS(fsinfo->size);
    uint start = 1 /* super block */;
    if (fsinfo->logstart != start) {
        xv6_error("expected logstart = 1, got %u", fsinfo->logstart);
        goto out_fail;
    }
    start += fsinfo->nlog;
    if (fsinfo->inodestart != start) {
        xv6_error("expected inode start = %u, got %u", start, fsinfo->inodestart);
        goto out_fail;
    }
    start += fsinfo->ninode_blocks;
    if (fsinfo->bmapstart != start) {
        xv6_error("expected bitmap start = %u, got %u", start, fsinfo->bmapstart);
        goto out_fail;
    }
    start += fsinfo->nbmap_blocks;
    start += fsinfo->nblocks;
    if (fsinfo->size < start) {
        xv6_error("Disk too small(%u) to hold %u blocks.", fsinfo->size, start);
        goto out_fail;
    } else if (fsinfo->size > start) {
        /* Wasted some disk blocks. */
        xv6_warn("Disk too large(%u) to hold %u blocks.", fsinfo->size, start);
    }
    /*
     * FIXME: Now check the bitmap blocks. All metadata blocks(super, inode, bitmap)
     * should be marked 1 in the bitmap, because they cannot be allocated for file 
     * or directory. 
     */

    /* Read root directory. */
    root_dir = new_inode(sb);
    error = -ENOMEM;
    if (!root_dir) {
        goto out_fail;
    }
    fsinfo->root_dir = root_dir;
    bh = sb_bread(sb, 1 /* Super block */ + fsinfo->nlog);
    if (bh == NULL) {
        error = -EIO;
        goto out_fail;
    }
    const struct dinode *xv6_root_dinode = (struct dinode *) bh->b_data + ROOTINO;
    if (__le16_to_cpu(xv6_root_dinode->type) != T_DIR) {
        error = -EINVAL;
        goto out_fail;
    }
    brelse(bh); bh = NULL;

	/* Apply parsed options to sbi (structure copy) */
    /* Finished without error. */
	fsinfo->options = *(const struct xv6_mount_options *)(fc->fs_private);
    return 0;
out_fail:
    kfree(fsinfo);
    if (bh) {
        brelse(bh);
    }
    if (root_dir) {
        iput(root_dir);
    }
    return error;
}

static int xv6_reconfigure(struct fs_context *fc) {
    sync_filesystem(fc->root->d_sb);
    return 0;
}

static int xv6_parse_param(struct fs_context *fc, struct fs_parameter *param) {
    struct fs_parse_result result;
    struct xv6_mount_options *options = fc->fs_private;

    int opt;
    opt = fs_parse(fc, xv6_param_spec, param, &result);
    if (opt < 0) {
        return opt;
    }

    switch (opt) {
        case (XV6_UID):
            options->uid = result.uid;
            break;
        case (XV6_GID):
            options->gid = result.gid;
            break;
        default: return -EINVAL;
    }
    return 0;
}

static int xv6_show_options(struct seq_file *m, struct dentry *root) {
    struct xv6_fs_info *fsinfo = root->d_sb->s_fs_info;
	struct xv6_mount_options *opts = &fsinfo->options;
    seq_printf(m, ",uid=%u",
            from_kuid_munged(&init_user_ns, opts->uid));
    seq_printf(m, ",gid=%u",
            from_kgid_munged(&init_user_ns, opts->gid));
    return 0;
}
