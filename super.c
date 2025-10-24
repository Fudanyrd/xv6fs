#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <asm/atomic.h>
#include <asm/uaccess.h>

#include "fsinfo.h"

static int xv6fs_init_fs_ctx(struct fs_context *fc) {
    fc->ops = &xv6fs_context_ops;
    void *buf = kzalloc(sizeof(struct xv6_mount_options), GFP_KERNEL);
    if (!buf) {
        return -ENOMEM;
    }
    fc->fs_private = buf;
    return 0;
}

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
    sb->s_root = NULL;

	set_default_d_op(sb, &xv6_dentry_ops);
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
        xv6_error("Bad magic number: 0x%x", 
            __le32_to_cpu(xv6_sb->magic));
        goto out_fail;
    }
    mutex_init(&fsinfo->build_inode_lock);
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
    fsinfo->balloc_hint = start; /* Initialize hint to the first data block. */
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
    root_dir->i_sb = sb;
    fsinfo->root_dir = NULL /* root_dir */;
    bh = sb_bread(sb, 1 /* Super block */ + fsinfo->nlog);
    if (bh == NULL) {
        error = -EIO;
        goto out_fail;
    }
    const struct dinode *xv6_root_dinode = (struct dinode *) bh->b_data + ROOTINO;
    if ((error = xv6_init_inode(root_dir, xv6_root_dinode, ROOTINO)) != 0) {
        goto out_fail;
    }
    if (__le16_to_cpu(xv6_root_dinode->type) != T_DIR) {
        error = -EINVAL;
        goto out_fail;
    }
    brelse(bh); bh = NULL;
    sb->s_root = d_make_root(root_dir);
	if (!sb->s_root) {
		xv6_error("get root inode failed");
		goto out_fail;
	}
    xv6_info("got root dentry 0x%lx", (unsigned long) sb->s_root);

	/* Apply parsed options to sbi (structure copy) */
    /* Finished without error. */
	fsinfo->options = *(const struct xv6_mount_options *)(fc->fs_private);
    xv6_info("Mounted xv6fs with uid=%u, gid=%u",
        from_kuid_munged(&init_user_ns, fsinfo->options.uid),
        from_kgid_munged(&init_user_ns, fsinfo->options.gid));
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

static int xv6_get_tree(struct fs_context *fc) {
    return get_tree_bdev(fc, xv6_fill_super);
}

static int xv6_reconfigure(struct fs_context *fc) {
	struct super_block *sb = fc->root->d_sb;
    bool new_readonly = fc->sb_flags & SB_RDONLY;
    if (new_readonly) {
        sb->s_flags |= SB_RDONLY;
    } else {
        sb->s_flags &= ~SB_RDONLY;
    }
    sync_filesystem(sb);
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
  
static void xv6_free_fc(struct fs_context *fc) {
    if (fc->fs_private) {
        kfree(fc->fs_private);
    }
}

static void xv6_kill_block_super(struct super_block *sb) {
    struct dentry *root = sb->s_root;
    if (root) {
        sb->s_root = NULL;
        xv6_info ("freeing root 0x%lx", (unsigned long) root);
        dput(root);
    }
    xv6_info ("Unmounting xv6fs");
    kill_block_super(sb);
}

static const struct super_operations xv6_super_ops = {
    .alloc_inode = xv6_alloc_inode,
    .free_inode = xv6_free_inode,
    .destroy_inode = xv6_free_inode,
    .show_options = xv6_show_options,
};
