#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>

#include "fs.h"
#include "fsinfo.h"

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

static int xv6_find_inum(struct inode *dir, struct dentry *entry, uint *inum) {
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
    *inum = 0;
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
            if (strncmp(entry->d_name.name, de[i].name, DIRSIZ) == 0) {
                *inum = __le16_to_cpu(de[i].inum);
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

static int xv6_dentry_alloc(struct inode *dir, const char *name, uint *num) {
    const uint nents = BSIZE / sizeof(struct dirent);
    if ((dir->i_mode & S_IFMT) != S_IFDIR) {
        /* Not a directory. */
        return -ENOTDIR;
    }

    if (*name == '.' && (name[1] == '\0' || 
            (name[1] == '.' && name[2] == '\0'))) {
        /* Do not allow creating "." or ".." entries. */
        return -EEXIST;
    }

    *num = 0;
    uint size = dir->i_size;
    uint block = 0;
    struct buffer_head *bh = NULL;
    int error;
    xv6_assert(size % sizeof(struct dirent) == 0 && "corrupted directory size");
    size /= sizeof(struct dirent);
    xv6_assert(size >= 2 && "directory must contain . and .. entries");
    while ((error = xv6_inode_block(dir, block, &bh)) == 0) {
        if (bh == NULL) {
            *num = block * nents;
            break; /* This is a virtual zeroed block. */
        }
        const struct dirent *de = (const struct dirent *) bh->b_data;
        const int lim = xv6_min(nents, size);
        for (int i = 0; i < lim; i++) {
            if (de[i].inum == 0) {
                *num = block * nents + i;
                break; /* unused entry */
            }
        }
        brelse(bh);
        block += 1;
        size -= lim;
        if (size == 0) {
            /* All entries have been checked. */
            break;
        }
    }
    return error;
}

static int xv6_readdir(struct file *dir, struct dir_context *ctx) {
    typeof(ctx->pos) *cpos = &ctx->pos;
    struct inode *inode = dir->f_inode;
    int error;

    /*
     * You should really understand the VFS's locking protocols,
     * it says: this is callled without any locks held! 😃
     *
     * Just like ext4_readdir, I do not try to hold locks,
     * because iterate_dir(which getdents relies on) already held
     * read lock.
     */
    
    if ((inode->i_mode & S_IFMT) != S_IFDIR) {
        error = -ENOTDIR;
        goto readdir_fini;
    }

    uint size = inode->i_size;
    xv6_assert(size % sizeof(struct dirent) == 0 && "corrupted directory size");
    size /= sizeof(struct dirent);

    uint block = *cpos / (BSIZE / sizeof(struct dirent));
    uint offset = *cpos % (BSIZE / sizeof(struct dirent));
    const int nents = BSIZE / sizeof(struct dirent);

    struct buffer_head *bh = NULL;
    while ((error = xv6_inode_block(inode, block, &bh)) == 0) {
        if (bh == NULL) {
            break;
        }
        const struct dirent *de = (const struct dirent *) bh->b_data;
        for (int i = offset; i < nents; i++) {
            if (de[i].inum == 0) {
                continue; /* unused entry */
            }
            if (!dir_emit(ctx, de[i].name, strnlen(de[i].name, DIRSIZ),
                        __le16_to_cpu(de[i].inum), DT_UNKNOWN)) {
                brelse(bh);
                goto readdir_fini;
            }
            (*cpos) += 1;
        }
        brelse(bh);
        bh = NULL;
        block += 1;
        offset = 0;
    }

readdir_fini:
    return error;
}

static int xv6_dentry_next(struct inode *dir, uint *num) {
    *num = 0;
    
    uint size = dir->i_size;
    const uint ndents = BSIZE / sizeof(struct dirent);
    xv6_assert(size % sizeof(struct dirent) == 0);
    size /= sizeof(struct dirent);
    size += 1;
    int error = 0;

    struct buffer_head *bh = NULL;
    error = xv6_inode_wblock(dir, size / ndents, &bh); 
    if (error) {
        return error;
    }
    brelse(bh);
    dir-> i_size = size * sizeof(struct dirent);
    *num = size - 1;

    return 0;
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
    memset(de->name, 0, DIRSIZ);
    strncpy(de->name, name, DIRSIZ);
    de->inum = __cpu_to_le16(inum);
    mark_buffer_dirty(bh);
    error = sync_dirty_buffer(bh);
    brelse(bh);
    return error;
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

static int xv6_dir_erase(struct inode *dir, int inum) {
    const int nents = BSIZE / sizeof(struct dirent);
    uint size = dir->i_size;
    int error;
    xv6_assert(size % sizeof(struct dirent) == 0 && "corrupted directory size");
    size /= sizeof(struct dirent);
    xv6_assert(size >= 2 && "directory must contain . and .. entries");
    struct buffer_head *bh = NULL;
    uint block = 0;

    while ((error = xv6_inode_block(dir, block, &bh)) == 0) {
        if (bh == NULL) {
            continue; /* This is a virtual zeroed block. Ignore */
        }
        struct dirent *de = (struct dirent *) bh->b_data;
        const int lim = xv6_min(nents, size);
        for (int i = 0; i < lim; i++) {
            if (de[i].inum == __cpu_to_le16(inum)) {
                de[i].inum = 0;
                de[i].name[0] = 0;
                goto derase_found;
            }
        }
        brelse(bh);
        block += 1;
        size -= lim;
        if (size == 0) {
            break;
        }
    }

    if (!error) {
        /* Not found. */
        error = -ENOENT;
    }
    return error;

derase_found:
    mark_buffer_dirty(bh);
    error = sync_dirty_buffer(bh);
    brelse(bh);
    return error;
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
