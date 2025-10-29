/*
 * Provides C++ methods used by the file system.
 */
#include "xv6c++.h"
#include "check.h"

#ifndef EFBIG
#  error "either include <errno.h> or <linux/fs.h>."
#endif

int xv6_inode_addr(struct checker *check, struct xv6_inode_ctx *inode,
            uint i, uint *blockno, bool alloc) {
    *blockno = 0;
    if (i > MAXFILE) {
        /* 
         * regardless whether we tries to allocate a block,
         * return an error. Otherwise, we'll get a buffer-overflow.
         */
        return -EFBIG;
    }

    int error = 0;
    uint *addrs = inode->addrs;
    inode->dirty = false;
    void *sb = check->privat; /* superblock */

    if (i < NDIRECT) {
        if (addrs[i] == 0 && alloc) {
            error = check->balloc(sb, &addrs[i]);
            if (error == 0 && addrs[i] == 0) {
                /* Balloc failed. */
                error = -ENOSPC;
            }
            inode->dirty = true;
        }

        *blockno = addrs[i];
        return error;
    }

    i -= NDIRECT;
    uint *indirno = &addrs[NDIRECT];
    if (*indirno == 0) {
        if (!alloc) {
            return 0;
        }
        inode->dirty = true;
        error = check->balloc(sb, indirno);
        if (error) {
            return error;
        }
        if (*indirno == 0) {
            return -ENOSPC;
        }
    }
    struct bufptr indir_buf(check->bread(sb, *indirno), check);
    if (indir_buf.buf_ == nullptr) { return -EIO; }
    uint *data = reinterpret_cast<uint *>(indir_buf.data());
    uint datano = __le32_to_cpu(data[i]);
    if (datano == 0) {
        if (!alloc) { return 0; }
        inode->dirty = true;
        error = check->balloc(sb, &datano);
        if (error) { return error; }
        if (datano == 0) { return -ENOSPC; }
        data[i] = __cpu_to_le32(datano);
        /* Should mark buffer to dirty. */
        (void) check->bflush(sb, indir_buf.buf_);
    }

    *blockno = datano;
    return 0;
}

int xv6_dir_iterate(struct checker *check,
            struct xv6_inode_ctx *dir, 
            xv6_diter_callback callback, /* iteration callback. */
            void *ctx /* context should be passed to callback. */,
            uint off, /* offset in entries. */
            bool rw) {
    auto size = dir->size;
    if (size % sizeof(struct dirent) != 0 || size < 2 * sizeof(struct dirent)) {
        check->panic("xv6: dir has incorrect size");
    }
    const uint nents = BSIZE / sizeof(struct dirent);
    size /= sizeof(struct dirent);

    if (off > size) {
        /* Reached the end. should check to avoid underflow */
        return 0;
    }
    size -= off;

    bool alloc = rw /* read-write enabled. */;
    uint i = off / nents;
    off = off % nents;
    struct xv6_diter_action act = xv6_diter_action_init;
    int error;
    uint blockno;
    while ((error = xv6_inode_addr(check, dir, i, &blockno, alloc)) == 0) {
        const uint lim = xv6_min(nents - off, size);
        if (blockno == 0) {
            uchar dummy[sizeof(struct dirent)] = {0};
            /* Most callback relies on it only called once. */
            act = callback(i * nents, (struct dirent *) &dummy, ctx);
            if (act.de_dirty) {
                check->warning("%s dentry should not be dirty", check->warn);
            }
            if (!act.cont) {
                break;
            }
        } else {
            struct bufptr debuf (check->bread(check->privat, blockno), check);
            if (debuf.buf_ == nullptr) {
                error = -EIO;
                break;
            }
            struct dirent *deptr = (struct dirent *) debuf.data();
            bool flush = false;
            for (uint k = off; k < lim; k++) {
                act = callback(i * nents + k, &deptr[k], ctx);
                flush |= act.de_dirty;
                if (!act.cont) {
                    break;
                }
            }
            if (flush) {
                error = check->bflush(check->privat, debuf.buf_);
            }
        }

        if (!act.cont || error) { break; }
        i++;
        size -= lim;
        off = 0;
        if (!size) { break;}
    }

    if (!error && act.cont && act.dir_ext) {
        /* operate on one more dentry. possibly used by insert. */
        size = dir->size;
        size /= sizeof(struct dirent);
        error = xv6_inode_addr(check, dir, size / nents, &blockno, alloc);
        if (error) { return error; }
        struct bufptr debuf (check->bread(check->privat, blockno), check);
        if (debuf.buf_ == nullptr) { return -EIO; }
        struct dirent *deptr = (struct dirent *) debuf.data();
        deptr += size % nents;
        act = callback(size, deptr, ctx);
        if (act.dir_dirty) {
            /* The insert method wants to extend inode. */
            dir->dirty = true;
            dir->size += sizeof(struct dirent);
        }
        if (act.de_dirty) {
            error = check->bflush(check->privat, debuf.buf_);
        }
    }

    return error;
}
