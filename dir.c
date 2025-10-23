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
    struct dinode dino;
    int error = xv6_dget(dir, &dino);
    if (error) {
        return error;
    }
    short type = __le16_to_cpu(dino.type);
    if (type != T_DIR) {
        return -ENOTDIR;
    }

    uint size = __le32_to_cpu(dino.size);
    xv6_assert(size % sizeof(struct dirent) == 0 && "corrupted directory size");
    *inum = 0;
    struct buffer_head *bh = NULL;
    uint block = 0;
    while ((error = xv6_file_block(dir->i_sb, &dino, block, &bh)) == 0) {
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
