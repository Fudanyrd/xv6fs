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

    uint size = dir->i_size;
    int error;
    xv6_assert(size % sizeof(struct dirent) == 0 && "corrupted directory size");
    *inum = 0;
    struct buffer_head *bh = NULL;
    uint block = 0;
    while ((error = xv6_inode_block(dir, block, &bh)) == 0) {
        if (bh == NULL) {
            break; /* end of file */
        }
        const struct dirent *de = (const struct dirent *) bh->b_data;
        int nents = BSIZE / sizeof(struct dirent);
        for (int i = 0; i < nents; i++) {
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
    }
    return error;
}

static int xv6_readdir(struct file *dir, struct dir_context *ctx) {
    uint *cpos = &ctx->pos;
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
