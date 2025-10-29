#ifndef _XV6_CPP_H
#define _XV6_CPP_H 1

#ifdef __cplusplus
extern "C" {
#endif /* C++ */

#include "common.h"

/* This inode context only keeps necessary info of an inode. */
struct xv6_inode_ctx {
    uint *addrs;  /**< Addresses */
    uint size;    /**< Size. */
    bool dirty;   /**< Is the inode dirty? */
};

#ifdef _LINUX_FS_H
struct inode;
#define xv6_inode_ctx_init(ino)        \
    {                                  \
        .size = ino->i_size,           \
        .dirty = false,                \
    }
#endif /* _LINUX_FS_H */

#ifndef __cpu_to_le32
static inline uint _cpp_to_le32(uint a) {
    uint ret = 0;
    uchar *dst = (uchar *) &ret;
    dst[0] = a & 0xff;
    dst[1] = (a >> 8) & 0xff;
    dst[2] = (a >> 16) & 0xff;
    dst[3] = (a >> 24);
    return ret;
}

#define __cpu_to_le32(x) (_cpp_to_le32(x))
#endif /* __cpu_to_le32 */

struct xv6_diter_action {
    unsigned char cont : 1, /* Should continue iteration */
        de_dirty: 1, /* dirent is dirty */
        dir_ext : 1, /* Do not stop at end of dir. */ 
        dir_dirty : 1, /* The inode of the dir is dirty; */
        padding : 4; /* Unused */
};

#define xv6_diter_action_init \
    {                         \
        .de_dirty = 0,        \
        .dir_ext = 0,         \
        .dir_dirty = 0,       \
        .padding = 0,         \
    }

typedef struct xv6_diter_action (*xv6_diter_callback)(
            uint dnum,         /* dirent number to locate dirent */
            struct dirent *de, /* The dirent for read/write */
            void *ctx /* Other context needed. */);

int xv6_dir_iterate(struct checker *check,
            struct xv6_inode_ctx *dir, 
            xv6_diter_callback callback, /* iteration callback. */
            void *ctx /* context should be passed to callback. */,
            uint off, /* offset, avoid slow readdir. */
            bool alloc);

/**
 * Get or allocate the address of the ith block.  
 * @param[out] blockno the LBA of it; 0 if not exist, or cannot allocate.
 */
int xv6_inode_addr(struct checker *check, struct xv6_inode_ctx *inode,
            uint i, uint *blockno, bool alloc);

#ifndef __le16_to_cpu
static inline ushort _cpp_to_cpu16(ushort a) {
    ushort b = 0;
    uchar *src = (uchar *) &a;
    b |= src[0];
    b |= (1u << 8) * src[1];
    return b;
}

#define __le16_to_cpu(x) (_cpp_to_cpu16(x))
#endif /* __le16_to_cpu */
#ifndef __le32_to_cpu
static inline uint _cpp_to_cpu32(uint a) {
    uint b = 0;
    uchar *src = (uchar *) &a;
    b |= src[0];
    b |= (1u << 8) * src[1];
    b |= (1u << 16) * src[2];
    b |= (1u << 24) * src[3];
    return b;
}

#define __le32_to_cpu(x) (_cpp_to_cpu32(x))
#endif /* __le32_to_cpu */

#ifdef __cplusplus
}
#endif /* C++ */

#endif /* _XV6_CPP_H 1 */
