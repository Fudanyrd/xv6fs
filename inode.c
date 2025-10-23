#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/iversion.h>
#include <linux/stat.h>

#include "fs.h"
#include "fsinfo.h"

extern struct dentry *d_splice_alias(struct inode *inode, struct dentry *dentry);

static int xv6_hash(const struct dentry *dentry, struct qstr *s) {
    const unsigned char *xv6_name = s->name;

    if (*xv6_name != 0) {
        s->hash = full_name_hash(dentry, xv6_name, DIRSIZ);
    }

    /* Success */
    return 0;
}

static int xv6_cmp(const struct dentry *dentry,
         unsigned int len, const char *str, const struct qstr *name) {
    __attribute__((unused)) struct super_block *sb = 
                dentry->d_sb; 
    
    int ret = 0;
    unsigned char a, b;
    const unsigned char *pa = name->name;
    const unsigned char *pb = str;
    for (int i = 0; i < DIRSIZ; i++) {
        a = pa[i]; 
        b = pb[i];
        if (a != b || (!a)) {
            ret = (int) a;
            ret -= b;
            break;
        }
    }
    return ret;
}

static int xv6_create(struct mnt_idmap *idmap, struct inode *dir,
            struct dentry *dentry, umode_t mode, bool extc) {
    /* FIXME: not implemented. */
    return -EINVAL;
}

static struct dentry *xv6_lookup(struct inode *dir, struct dentry *dentry,
			 unsigned int flags) {
    struct super_block *sb = dir->i_sb;
    xv6_lock(sb);
	struct inode *inode = NULL;

    uint inum = 0;
    int reason = xv6_find_inum(dir, dentry, &inum);
    if (!inum) {
        /* Not found */
        inode = ERR_PTR(-ENOENT);
    } else if (reason) {
        /* Some error occurred */
        inode = ERR_PTR(reason);
    } else {
        /* Load the inode from disk (may fail) */
        inode = xv6_iget(sb, inum);
    }

    xv6_unlock(sb);
    /* d_splice_alias will use the `IS_ERR` macro to check inode. */
    return d_splice_alias(inode, dentry);
}

static struct inode *xv6_iget(struct super_block *sb, uint inum) {
    /* Get xv6's metadata, and address the target inode. */
    const struct xv6_fs_info *fsinfo = (const struct xv6_fs_info *) 
                (sb->s_fs_info);
    uint inode_start = fsinfo->inodestart;
    xv6_assert(inum != 0 && inum < fsinfo->ninodes);

    const struct dinode *disk_inode;
    uint inode_block = inode_start + inum / IPB;
    struct buffer_head *bh = NULL;
    int error;
    struct inode *inode = NULL;

    /* Load the inode from disk. */
    bh = sb_bread(sb, inode_block);
    if (bh == NULL) {
        return ERR_PTR(-EIO);
    }

    inode = new_inode(sb);
    if (inode == NULL) {
        error = (-ENOMEM);
        goto iget_fini;
    } /* need to free(inode) or return inode. */
    disk_inode = (const struct dinode *) (bh->b_data);
    disk_inode += inum % IPB;
    inode->i_ino = inum;
    error = xv6_init_inode(inode, disk_inode);

iget_fini:
    brelse(bh);
    if (error) {
        if (inode) { iput(inode); }
        return ERR_PTR(error);
    }
    return inode;
}

static int xv6_init_inode(struct inode *ino, const struct dinode *dino) {
    struct super_block *sb = ino->i_sb;
    const struct xv6_fs_info *fsinfo = (const struct xv6_fs_info *) 
                (sb->s_fs_info);
    xv6_assert(ino != NULL && fsinfo != NULL);

    ino->i_uid = fsinfo->options.uid;
    ino->i_gid = fsinfo->options.gid;
    ino->i_op = &xv6_inode_ops;
	inode_inc_iversion(ino);
	ino->i_generation = get_random_u32();
    set_nlink(ino, __le16_to_cpu(dino->nlink));
    typeof(ino->i_mode) mode = S_IRWXU | S_IRWXG | S_IRWXO;
    if (sb->s_flags & SB_RDONLY) {
        mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
    }

    ushort itype = __le16_to_cpu((ushort) dino->type);
    bool isdir = false;
    switch (itype) {
        case T_DIR: isdir = true;
            break;
        case T_FILE:
        case T_DEVICE:
        default: {
            xv6_error("inode %lu: Unsupported inode type %hu\n",  ino->i_ino, itype);
            return -EINVAL;
        }
    }

    if (isdir) {
        mode |= S_IFDIR;
		ino->i_generation &= ~1;
    } else {
		ino->i_generation |= 1;
        mode |= S_IFREG;
    }

    /* For simplicity, set them to 1970-01-01. */
    ino->i_atime_sec = ino->i_mtime_sec = ino->i_ctime_sec = 0;
    ino->i_atime_nsec = ino->i_mtime_nsec = ino->i_ctime_nsec = 0;

    return 0;
}

static int xv6_create(struct mnt_idmap *idmap, struct inode *dir,
            struct dentry *dentry, umode_t mode, bool extc);

static int xv6_getattr(struct mnt_idmap *idmap, const struct path *path,
		      struct kstat *stat, u32 request_mask, unsigned int flags) {
	struct inode *inode = d_inode(path->dentry);
	generic_fillattr(idmap, request_mask, inode, stat);
	stat->blksize = BSIZE;
    stat->ino = inode->i_ino;
    return 0;
}
/* xv6's inode operation struct. '*/
static const struct inode_operations xv6_inode_ops = {
    .lookup = xv6_lookup,
    .create = xv6_create,
    .update_time = xv6_update_time,
    .permission = NULL,
    .getattr = xv6_getattr,
};

/* comparison */
static const struct dentry_operations xv6_dentry_ops = {
    .d_hash = xv6_hash,
    .d_compare = xv6_cmp,
};

