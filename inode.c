#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/iversion.h>
#include <linux/stat.h>

#include "fs.h"
#include "fsinfo.h"
#include "xv6.h"

extern struct dentry *d_splice_alias(struct inode *inode, struct dentry *dentry);

static int xv6_ialloc(uint *inum, struct super_block *sb, 
            const struct dinode *dino) {
    *inum = 0;
    const struct xv6_fs_info *fsinfo = (const void *) sb->s_fs_info;
    uint node = 2 /* skip null and root. */;
    const uint block_inodes = BSIZE / sizeof(*dino);
    const uint blockend = fsinfo->bmapstart;
    uint block = fsinfo->inodestart;
    uint off = node % block_inodes;
    struct buffer_head *bh = NULL;
    int error = 0;

    xv6_lock_itable(sb);
    for (; block < blockend; block++) {
        bh = sb_bread(sb, block);
        if (!bh) {
            error = -EIO;
            break;
        }

        struct dinode *dptr = (struct dinode *) bh->b_data;
        for (uint i = off; i < block_inodes; i++, node++) {
            if (dptr[i].type == 0) {
                /* Found an unused inode. */
                if (dino) {
                    memcpy(&dptr[i], dino, sizeof(*dino));
                    mark_buffer_dirty(bh);
                    error = sync_dirty_buffer(bh);
                }
                if (error) {
                    break;
                }
                *inum = node;
                break;
            }
        }

        brelse(bh);
        if (*inum != 0 || error) {
            break;
        }
        off = 0;
    }
    xv6_unlock_itable(sb);

    if (*inum == 0 && error == 0)
        error = -ENOSPC; 
    return error;
}

static int xv6_ifree(struct super_block *sb, uint inum) {
    struct buffer_head *bh = NULL;
    int error = 0;
    const struct xv6_fs_info *fsinfo = (const void *) sb->s_fs_info;
    const uint inodestart = fsinfo->inodestart;

    xv6_lock_itable(sb);
    uint block = inodestart + inum / IPB;
    bh = sb_bread(sb, block);
    if (!bh) {
        error = -EIO;
        goto ifree_fini;
    }

    struct dinode *dptr = (struct dinode *) bh->b_data;
    dptr += inum % IPB;
    memset(dptr, 0, sizeof(*dptr));
    mark_buffer_dirty(bh);
    error = sync_dirty_buffer(bh);
    brelse(bh); 

ifree_fini:
    xv6_unlock_itable(sb);
    return error;
}

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

static struct dentry *xv6_lookup(struct inode *dir, struct dentry *dentry,
			 unsigned int flags) {
    struct super_block *sb = dir->i_sb;
	struct inode *inode = NULL;

    uint dnum = 0;
    struct dirent de;
    int reason = xv6_find_inum(dir, dentry->d_name.name, &dnum, &de);
    if (!dnum) {
        /* Not found. Like vfat_lookup, should return null. */
        return NULL;
    } else if (reason) {
        /* Some error occurred */
        inode = ERR_PTR(reason);
    } else {
        /* Load the inode from disk (may fail) */
        uint inum = __le16_to_cpu(de.inum);
        xv6_assert(inum != 0 && "should not get null inode");
        inode = xv6_iget(sb, inum);
    }

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
    int error = 0;
    bool found = false;
    struct inode *inode = xv6_find_inode(sb, inum, &found);
    if (inode == NULL) {
        error = -ENOMEM;
        goto iget_fini;
    }
    if (found) { goto iget_fini; } /* can skip initialization. */

    /* Load the inode from disk. */
    bh = sb_bread(sb, inode_block);
    if (bh == NULL) {
        error = -EIO;
        goto iget_fini;
    }

    disk_inode = (const struct dinode *) (bh->b_data);
    disk_inode += inum % IPB;
    inode->i_ino = inum;
    error = xv6_init_inode(inode, disk_inode, inum);
    brelse(bh);

iget_fini:
    if (error) {
        if (inode) { iput(inode); }
        return ERR_PTR(error);
    }
    return inode;
}

static int xv6_init_inode(struct inode *ino, const struct dinode *dino, uint inum) {
    struct super_block *sb = ino->i_sb;
    const struct xv6_fs_info *fsinfo = (const struct xv6_fs_info *) 
                (sb->s_fs_info);
    xv6_assert(ino != NULL && fsinfo != NULL);

    ino->i_ino = inum;
    ino->i_uid = fsinfo->options.uid;
    ino->i_gid = fsinfo->options.gid;
    ino->i_op = &xv6_inode_ops;
    ino->i_fop = &xv6_file_ops;
    init_rwsem(&ino->i_rwsem);
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
        case T_DEVICE: break;
        default: {
            xv6_error("inode %lu: Unsupported inode type %hu\n",  ino->i_ino, itype);
            return -EINVAL;
        }
    }

    if (isdir) {
        mode |= S_IFDIR;
		ino->i_generation &= ~1;
        ino->i_fop = &xv6_directory_ops;
    } else {
		ino->i_generation |= 1;
        mode |= S_IFREG;
    }

    /* For simplicity, set them to 1970-01-01. */
    ino->i_atime_sec = ino->i_mtime_sec = ino->i_ctime_sec = 0;
    ino->i_atime_nsec = ino->i_mtime_nsec = ino->i_ctime_nsec = 0;
    ino->i_mode = mode;
    ino->i_size = __le32_to_cpu(dino->size);
    struct xv6_inode_info *i_info = kmalloc(sizeof(struct xv6_inode_info), GFP_KERNEL);
    if (!i_info) {
        return -ENOMEM;
    }
    uint *addrs = i_info->addrs;
    for (int i = 0; i < NDIRECT + 1; i++) {
        addrs[i] = __le32_to_cpu(dino->addrs[i]);
    }
    ino->i_private = i_info;
    insert_inode_hash(ino);

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

static int xv6_sync_inode(struct inode *ino) {
    struct super_block *xv6_sb = ino->i_sb;
    if (xv6_sb->s_flags & SB_RDONLY) {
        return 0;
    }

    const struct xv6_fs_info *fsinfo = xv6_sb->s_fs_info;
    const uint inum = ino->i_ino;
    xv6_assert(inum && "null inode found");
    uint block = fsinfo->inodestart + inum / IPB;

    struct buffer_head *bh = sb_bread(xv6_sb, block);
    if (bh == NULL) {
        return -EIO;
    }
    struct dinode *dptr = (struct dinode *) bh->b_data;
    dptr += inum % IPB;

    if (ino->i_private) {
        uint *addrs = ((struct xv6_inode_info *) ino->i_private)->addrs;
        for (int i = 0; i < NDIRECT + 1; i++) {
            dptr->addrs[i] = __cpu_to_le32(addrs[i]);
        }
    } else {
        xv6_warn("inode %lu has no private data\n", ino->i_ino);
    }
    dptr->size = __cpu_to_le32((uint) ino->i_size);
    dptr->nlink = __cpu_to_le16((ushort) ino->i_nlink);
    mark_buffer_dirty(bh);
    int error = sync_dirty_buffer(bh);
    brelse(bh);
    return error;
}

static void xv6_evict_inode(struct inode *ino) {
   /*
    * https://elixir.bootlin.com/linux/v6.17.4/source/fs/autofs/inode.c#L105
    */
    xv6_debug("evicting inode %lu", ino->i_ino);
    truncate_inode_pages_final(&ino->i_data);
    clear_inode(ino);
    kfree(ino->i_private);
    ino->i_private = NULL;
}

static int xv6_inode_block(struct inode *ino, uint i,
            struct buffer_head **bhptr) {
    struct super_block *sb = ino->i_sb;
    uint *addrs;
    struct dinode dino;
    int error;
    if (i >= MAXFILE) {
        goto file_end;
    }

    /* Set `addrs` to start of indexing array.  */
    if (unlikely(ino->i_private == NULL)) {
        /* warn potential ENOMEM */
        error = xv6_dget(ino, &dino);
        if (error) { return error; }
        for (int i = 0; i < NDIRECT + 1; i++) {
            dino.addrs[i] = __le32_to_cpu(dino.addrs[i]);
        }
        addrs = dino.addrs;
    } else {
        addrs = ((struct xv6_inode_info *) ino->i_private)->addrs;
    }

    *bhptr = NULL;
    struct buffer_head *bh;
    struct buffer_head *indirect = NULL;
    if (i < NDIRECT) {
        uint block = addrs[i];
        if (block == 0) { goto file_end; }
        bh = sb_bread(sb, block);
        *bhptr = bh;
        return bh ? 0 : -EIO;
        /* FIXME: check block later */
    }

    i -= NDIRECT;   
    uint indirect_block = (addrs[NDIRECT]);
    if (indirect_block == 0) { goto file_end; }

    indirect = sb_bread(sb, indirect_block);
    if (indirect == NULL) { 
        return -EIO;
    }
    const uint *iaddrs = (const uint *) indirect->b_data;
    uint data_block = __le32_to_cpu(iaddrs[i]);
    brelse(indirect);
    if (data_block == 0) {
        goto file_end;
    }

    bh = sb_bread(sb, data_block);
    *bhptr = bh;
    return bh ? 0 : -EIO;
file_end:
    *bhptr = 0;
    return 0;
}

static int xv6_inode_wblock(struct inode *ino, uint i,
            struct buffer_head **bhptr) {
    if (i >= MAXFILE) {
        return -EFBIG;
    }

    uint *addrs;
    struct dinode dino;
    int error = 0;
    bool dirty = false;
    struct buffer_head *buf_indirect = NULL;
    struct buffer_head *buf_data = NULL;
    bool indirect_dirty = false;

    /* Set `addrs` to start of indexing array.  */
    if (unlikely(ino->i_private == NULL)) {
        /* warn potential ENOMEM */
        error = xv6_dget(ino, &dino);
        if (error) { return error; }
        for (int i = 0; i < NDIRECT + 1; i++) {
            dino.addrs[i] = __le32_to_cpu(dino.addrs[i]);
        }
        addrs = dino.addrs;
    } else {
        addrs = ((struct xv6_inode_info *) ino->i_private)->addrs;
    }

    if (i < NDIRECT) {
        if (addrs[i] == 0) {
            error = xv6_balloc(ino->i_sb, &addrs[i]);
            if (error) { return error; }
            if (addrs[i] == 0) { return -ENOSPC; }
            dirty = true;
        }
        *bhptr = sb_bread(ino->i_sb, addrs[i]);
        error = (*bhptr) ? 0 : -EIO;
        goto wblock_clean;
    }

    if (addrs[NDIRECT] == 0) {
        error = xv6_balloc_zero(ino->i_sb, &addrs[NDIRECT]);
        if (error) { return error; }
        if (addrs[NDIRECT] == 0) { return -ENOSPC; }
        dirty = true;
    }
    buf_indirect = sb_bread(ino->i_sb, addrs[NDIRECT]);
    if (!buf_indirect) {
        error = -EIO;
        goto wblock_clean_indir;
    }
    uint *iaddrs = (uint *) buf_indirect->b_data;
    i -= NDIRECT;
    if (iaddrs[i] == 0) {
        error = xv6_balloc(ino->i_sb, &iaddrs[i]);
        if (error) { goto wblock_clean_indir; }
        if (iaddrs[i] == 0) {
            error = -ENOSPC;
            goto wblock_clean_indir;
        }
        indirect_dirty = true;
    }
    buf_data = sb_bread(ino->i_sb, iaddrs[i]);
    *bhptr = buf_data;
    error = buf_data ? 0 : -EIO;

wblock_clean_indir:
    if (likely(buf_indirect)) {
        if (indirect_dirty)  {
            mark_buffer_dirty(buf_indirect);
            sync_dirty_buffer(buf_indirect);
        }
        brelse(buf_indirect);
    }
wblock_clean:
    if (dirty) {
        mark_inode_dirty(ino);
        if (unlikely(ino->i_private == NULL)) {
            /* write this inode to disk. */
            xv6_assert(offsetof(struct xv6_inode_info, addrs) == 0);
            ino->i_private = addrs;
            /* sync method depends on i_private to work properly. */
            error = xv6_sync_inode(ino);
            ino->i_private = NULL;
        } else {
            /* rely on fsync write this inode to disk. */
        }
    }
    return error;
}

static int xv6_create(struct mnt_idmap *idmap, struct inode *dir,
            struct dentry *dentry, umode_t mode, bool extc) {
    const char *name = dentry->d_name.name;
    if (strlen(name) > DIRSIZ) {
        return -ENAMETOOLONG;
    }

    uint inum;
    struct super_block *sb = dir->i_sb;
    int error = 0;
    struct dinode dino;
    struct inode *newinode = new_inode(sb);
    bool isdir;
    if (newinode == NULL) {
        return -ENOMEM;
    }

    memset(&dino, 0, sizeof(dino));
    dino.nlink = __cpu_to_le16(1);
    if ((mode & S_IFMT) == S_IFDIR) {
        dino.type = __cpu_to_le16(T_DIR);
        isdir = true;
        dino.size = sizeof(struct dirent) * 2; /* . and .. */
    } else {
        dino.type = __cpu_to_le16(T_FILE);
        isdir = false;
    }

    /* Try to allocate inode and allocate an data block. */
    if (true) {
        error = xv6_balloc(sb, dino.addrs);
        if (dino.addrs[0] == 0) { error = -ENOSPC; }
        dino.addrs[0] = __cpu_to_le32(dino.addrs[0]);
    }
    if (error) { goto create_fini; }

    error = xv6_ialloc(&inum, sb, &dino);
    if (error) { goto create_fini; }
    if (isdir) {
        error = xv6_dir_init(sb, __le32_to_cpu(dino.addrs[0]), 
                    dir->i_ino, inum);
        if (error) {
            goto create_fini;
        }
    }

    error = xv6_init_inode(newinode, &dino, inum);
    if (error) {
        goto create_fini;
    }
    
    error = xv6_dentry_insert(dir, name, inum);

create_fini:
    if (error) {
        iput(newinode);
    } else {
        /* Add the inode to the tree, without checking. */
        struct xv6_fs_info *fsinfo = (struct xv6_fs_info *)(sb->s_fs_info);
        struct xv6_inode *xi = container_of(newinode, struct xv6_inode, inode);
        mutex_lock(&fsinfo->itree_lock);
        rb_add(&xi->rbnode, &fsinfo->inode_tree, xv6_rb_less);
        mutex_unlock(&fsinfo->itree_lock);
        d_instantiate(dentry, newinode);
    }
    return error;
}

struct dentry *xv6_mkdir (struct mnt_idmap *mmap, struct inode *dir, 
            struct dentry *dentry, umode_t mode) {
    int error = xv6_create(NULL, dir, dentry, 
                S_IFDIR | 0777, true);
    return error ? ERR_PTR(error) : NULL;
}

static int xv6_setattr (struct mnt_idmap *a1, struct dentry *a2, 
            struct iattr *a3) {
    return 0;
}

static int xv6_inode_clear(struct inode *inode) {
    int error= 0;
    struct super_block *sb = inode->i_sb;
    struct dinode dino;
    uint *addrs;
    if (unlikely(inode->i_private == NULL)) {
        /* warn potential ENOMEM */
        error = xv6_dget(inode, &dino);
        if (error) { return error; }
        for (int i = 0; i < NDIRECT + 1; i++) {
            dino.addrs[i] = __le32_to_cpu(dino.addrs[i]);
        }
        addrs = dino.addrs;
    } else {
        struct xv6_inode_info *tmp = inode->i_private;
        addrs = tmp->addrs;
    }

    for (int i = 0; i < NDIRECT; i++) {
        if (addrs[i] != 0) {
            (void) xv6_bfree(sb, addrs[i]);
            addrs[i] = 0;
        }
    }

    uint iaddr = addrs[NDIRECT];
    if (iaddr) {
        struct buffer_head *bh = sb_bread(sb, iaddr);
        if (bh == NULL) {
            return -EIO;
        }
        uint *iaddrs = (uint *) bh->b_data;
        for (int i = 0; i < NINDIRECT; i++) {
            if (iaddrs[i] != 0) {
                (void) xv6_bfree(sb, __le32_to_cpu(iaddrs[i]));
            }
        }
        brelse(bh);
        (void) xv6_bfree(sb, iaddr);
    }
    addrs[NDIRECT] = 0;

    inode->i_size = 0;
    mark_inode_dirty(inode);
    if (unlikely(inode->i_private == NULL)) {
        /* write this inode to disk. */
        xv6_assert(offsetof(struct xv6_inode_info, addrs) == 0);
        inode->i_private = addrs;
        /* sync method depends on i_private to work properly. */
        error = xv6_sync_inode(inode);
        inode->i_private = NULL;
    } else {
        error = xv6_sync_inode(inode);
    }
    return error;
}

/* xv6's inode operation struct. '*/
static const struct inode_operations xv6_inode_ops = {
    .lookup = xv6_lookup,
    .create = xv6_create,
    .update_time = xv6_update_time,
    .permission = NULL,
    .getattr = xv6_getattr,
    .setattr = xv6_setattr,
    .mkdir = xv6_mkdir,
    .rmdir = xv6_rmdir,
    .link = xv6_link,
    .unlink = xv6_unlink,
    .rename = xv6_rename,
};

/* comparison */
static const struct dentry_operations xv6_dentry_ops = {
    .d_hash = xv6_hash,
    .d_compare = xv6_cmp,
};

