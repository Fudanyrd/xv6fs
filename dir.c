#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>

#include "fs.h"
#include "fsinfo.h"
#include "xv6.h"
#include "xv6c++.h"

static inline int xv6_dget(struct inode *dir, struct dinode *dino) {
    struct super_block *xv6_sb = dir->i_sb;
    const struct xv6_fs_info *fsinfo = xv6_sb->s_fs_info;
    const uint inum = dir->i_ino;
    xv6_assert(inum && "null inode found");
    uint block = fsinfo->inodestart + inum / IPB;

    struct buffer_head *bh = sb_bread(xv6_sb, block);
    if (bh == NULL) {
        return -EIO;
    }
    struct dinode *dptr = (struct dinode *) bh->b_data;
    dptr += inum % IPB;

    /* This routine does not check content of dinode. */
    memcpy(dino, dptr, sizeof(*dino));
    brelse(bh);
    return 0;
}

static int xv6_find_inum(struct inode *dir, const char *name, uint *dnum,
            struct dirent *dout) {
    if ((dir->i_mode & S_IFMT) != S_IFDIR) {
        /* Not a directory. */
        return -ENOTDIR;
    }

    const int nents = BSIZE / sizeof(struct dirent);
    uint size = dir->i_size;
    int error;
    xv6_assert(size % sizeof(struct dirent) == 0 && "corrupted directory size");
    size /= sizeof(struct dirent);
    xv6_assert(size >= 2 && "directory must contain . and .. entries");
    *dnum = 0;
    struct buffer_head *bh = NULL;
    uint block = 0;
    while ((error = xv6_inode_block(dir, block, &bh)) == 0) {
        if (bh == NULL) {
            continue; /* This is a virtual zeroed block. */
        }
        const struct dirent *de = (const struct dirent *) bh->b_data;
        const int lim = xv6_min(nents, size);
        for (int i = 0; i < lim; i++) {
            if (de[i].inum == 0) {
                continue; /* unused entry */
            }
            if (strncmp(name, de[i].name, DIRSIZ) == 0) {
                *dnum = block * nents + i;
                memcpy(dout, &de[i], sizeof(*dout));
                brelse(bh);
                return 0;
            }
        }
        brelse(bh);
        block += 1;
        size -= lim;
        if (size == 0) {
            break;
        }
    }
    return error;
}

static int xv6_dentry_insert(struct inode *dir, const char *name, uint inum) {
    const uint nents = BSIZE / sizeof(struct dirent);
    if ((dir->i_mode & S_IFMT) != S_IFDIR) {
        /* Not a directory. */
        return -ENOTDIR;
    }

    if (strlen(name) > DIRSIZ) {
        return -ENAMETOOLONG;
    }
    if (strcmp(name, ".") == 0 || strcmp("..", name) == 0) {
        /* Enforce this check. */
        return -EEXIST;
    }

    uint dnum = 0;
    uint size = dir->i_size;
    uint block = 0;
    struct buffer_head *bh = NULL;
    int error;
    xv6_assert(size % sizeof(struct dirent) == 0 && "corrupted directory size");
    size /= sizeof(struct dirent);
    xv6_assert(size >= 2 && "directory must contain . and .. entries");

    /* First try find an empty directory from 0 to size. */
    while ((error = xv6_inode_wblock(dir, block, &bh)) == 0) {
        if (unlikely(bh == NULL)) {
            error = -EIO;
            goto insert_fini;
        }
        const struct dirent *de = (const struct dirent *) bh->b_data;
        const int lim = xv6_min(nents, size);
        for (int i = 0; i < lim; i++) {
            if (de[i].inum == 0) {
                dnum = block * nents + i;
                break; /* unused entry */
            }
        }
        if (dnum != 0) { break; }
        brelse(bh);
        bh = NULL;
        block += 1;
        size -= lim;
        if (size == 0) {
            /* All entries have been checked. */
            break;
        }
    }

    if (dnum != 0) {
        goto insert_found;
    }

    /* Must extend the file, and mark dir dirty. */
    size  = dir->i_size;
    block = size / nents;
    dnum  = size / sizeof(struct dirent);
    dir->i_size = size + sizeof(struct dirent);
    mark_inode_dirty(dir);
    (void) xv6_sync_inode(dir);
    error = xv6_inode_wblock(dir, block, &bh);
    if (error) {
        goto insert_fini;
    }

insert_found:
    xv6_assert (bh != NULL);
    /* Write the entry. Now dnum = the allocated entry. */
    struct dirent *target = (struct dirent *) bh->b_data;
    target += dnum % nents;
    memset(target, 0, sizeof(*target));
    strncpy(target->name, name, DIRSIZ);
    target->inum = __cpu_to_le16((ushort) inum);

    /* Sync buffer. */
    mark_buffer_dirty(bh);
    error = sync_dirty_buffer(bh);
    brelse(bh);
    bh = NULL;
insert_fini:
    return error;
}

static struct xv6_diter_action readdir_callback(uint dnum, struct dirent *de,
            void *ctx) {
    struct xv6_diter_action next = xv6_diter_action_init;
    next.cont = 1;

    struct dir_context *dc = ctx;
    if (dnum < dc->pos) {
        return next;
    }
    /* else dnum >= dc->pos */
    bool cont;
    if (de->inum == 0) {
        /* Empty entry */
        cont = true;
    } else {
        cont = dir_emit(ctx, de->name, strnlen(de->name, DIRSIZ),
                    __le16_to_cpu(de->inum), DT_UNKNOWN);
    }
    dc->pos = dnum + (int)cont;
    next.cont = cont;
    return next;
}

static int xv6_readdir(struct file *dir, struct dir_context *ctx) {
    struct inode *inode = dir->f_inode;

    /*
     * You should really understand the VFS's locking protocols,
     * it says: this is callled without any locks held! 😃
     *
     * Just like ext4_readdir, I do not try to hold locks,
     * because iterate_dir(which getdents relies on) already held
     * read lock.
     */
    
    if ((inode->i_mode & S_IFMT) != S_IFDIR) {
        return -ENOTDIR;
    }

    struct super_block *sb = inode->i_sb;
    struct xv6_fs_info *fsinfo = sb->s_fs_info;
    struct xv6_inode_ctx ictx = xv6_inode_ctx_init(inode);
    int error;
    struct dinode di;
    if ((error = xv6_init_ictx(&ictx, inode, &di)) != 0) {
        return error;
    }
    int ret = xv6_dir_iterate(&fsinfo->check, &ictx, readdir_callback, ctx, 
                    ctx->pos, false);
    return ret;
}

static int xv6_dir_init(struct super_block *sb, uint block, 
            uint inum_parent, uint inum_this) {
    struct buffer_head *bh;
    int error;

    bh = sb_bread(sb, block);
    if (unlikely(bh == NULL)) {
        (void) xv6_bfree(sb, block);
        return -EIO;
    }
    struct dirent *de = (struct dirent *) bh->b_data;
    
    /* Insert . */
    strcpy(de->name, "."); 
    de->inum = __cpu_to_le16(inum_this);
    de ++;

    /* Insert .. */
    strcpy(de->name, ".."); 
    de->inum = __cpu_to_le16(inum_parent);

    /* Flush data. */
    mark_buffer_dirty(bh);
    error = sync_dirty_buffer(bh);
    brelse(bh);

    if (error) {
        (void) xv6_bfree(sb, block);
    }
    return error;
}

static int xv6_dir_erase(struct inode *dir, const char *name) {
    uint size = dir->i_size;
    int error;
    xv6_assert(size % sizeof(struct dirent) == 0 && "corrupted directory size");
    size /= sizeof(struct dirent);
    xv6_assert(size >= 2 && "directory must contain . and .. entries");

    uint dnum = 0;
    struct dirent de;
    error = xv6_find_inum(dir, name, &dnum, &de);
    if (error) {
        return error;
    }
    if (dnum == 0) {
        return -ENOENT;
    }
    return xv6_dentry_write(dir, dnum, NULL, 0);
}

/* Test whether this directory can be safely removed. */
static int xv6_dir_rmtest(struct inode *dir) {

    bool ret = true;
    const int nents = BSIZE / sizeof(struct dirent);
    uint size = dir->i_size;
    int error;
    xv6_assert(size % sizeof(struct dirent) == 0 && "corrupted directory size");
    size /= sizeof(struct dirent);
    xv6_assert(size >= 2 && "directory must contain . and .. entries");
    struct buffer_head *bh = NULL;
    uint block = 0;
    uint boff = 2; /* Should ignore . and .. */
    while ((error = xv6_inode_block(dir, block, &bh)) == 0) {
        if (bh == NULL) {
            continue; /* This is a virtual zeroed block. */
        }
        const struct dirent *de = (const struct dirent *) bh->b_data;
        const int lim = xv6_min(nents - boff, size);
        for (int i = boff; i < lim; i++) {
            if (de[i].inum == 0) {
                continue; /* unused entry */
            }
            ret = false; break;
        }
        brelse(bh);
        block += 1;
        size -= lim;
        boff = 0;
        if (size == 0 || !ret) {
            break;
        }
    }

    if (error) {
        /* oops, do not try to do anything instead. */
        return error;
    }
    return ret ? 0 : -ENOTEMPTY;
}

static int xv6_rmdir(struct inode *dir, struct dentry *entry) {
    if ((dir->i_mode & S_IFMT) != S_IFDIR) {
        return -ENOTDIR;
    }
    if ((entry->d_inode->i_mode & S_IFMT) != S_IFDIR) {
        return -ENOTDIR;
    }

    int error = xv6_dir_rmtest(entry->d_inode);
    if (error) {
        /* Directory not empty, or other error. */
        return error;
    }

    return xv6_unlink(dir, entry);
}

static int xv6_dentry_write(struct inode *dir, uint dnum, const char *name, 
            uint inum) {
    const uint ndents = BSIZE / sizeof(struct dirent);
    struct buffer_head *bh;
    int error = 0;

    error = xv6_inode_wblock(dir, dnum / ndents, &bh); 
    if (error) {
        return error;
    }

    struct dirent *de = (struct dirent *) bh->b_data;
    de += dnum % ndents;
    if (name != NULL) {
        memset(de->name, 0, DIRSIZ);
        strncpy(de->name, name, DIRSIZ);
        de->inum = __cpu_to_le16(inum);
    } else {
        /* Do clear action instead. */
        memset(de, 0xfd, sizeof(*de));
        de->inum = 0;
        de->name[0] = 0;
    }
    mark_buffer_dirty(bh);
    error = sync_dirty_buffer(bh);
    brelse(bh);
    return error;
}

static int xv6_init_ictx(struct xv6_inode_ctx *ictx, struct inode *inode, 
                struct dinode *di) {
    if (likely(inode->i_private)) {
        struct xv6_inode_info *ii = inode->i_private;
        ictx->addrs = ii->addrs;
    } else {
        int error;
        if ((error = xv6_dget(inode, di)) != 0) {
            return error;
        }
        uint *addrs = di->addrs;
        for (int i = 0; i <= NDIRECT; i++) {
            addrs[i] = __le32_to_cpu(addrs[i]);
        }
        ictx->addrs = addrs;
    }
    return 0;
}
