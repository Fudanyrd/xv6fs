#include "xv6c++.h"
#include "check.h"
#include "defer.h"

struct xv6_checker_info {
    uint logstart;
    uint inodestart;
    uint bmapstart;
    uint datastart;
    uint logsize;
    uint inodesize;
    uint bmapsize;
    uint datasize;
    uint fssize;

    xv6_checker_info() = default;
    ~xv6_checker_info() = default;
};

/* Little-endian to CPU. */
static uint xuint(uint x) {
    uint ret = 0;
    uint mask = 1;
    unsigned char *pt = reinterpret_cast<unsigned char *>(&x);
    for (int i = 0; i < 4; i++) {
        ret |= mask * pt[i];
        mask *= 256;
    }
    return ret;
}

/* These checks are also done by kernel module (repetitive code 😭😭) */
static bool 
xv6_check_sb(const struct superblock *sb, struct checker *check, 
                xv6_checker_info *info) noexcept {
    bool check_size = true;
    uint fssize = xuint(sb->size);

    /* Check logging layer */
    unsigned long size = 1;
    uint logstart = xuint(sb->logstart);
    if (logstart != size) {
        check->error("%s expected logstart = %u, got %u\n", 
                    check->err, size, logstart);
        /* Just continue. */
        check_size = false;
    }
    size += xuint(sb->nlog);
    info->logsize = xuint(sb->nlog);
    info->logstart = 1;
    
    /* Check inode layer */
    uint inodestart = xuint(sb->inodestart);
    uint ninodes = xuint(sb->ninodes);
    uint ninode_blocks = INODE_BLOCKS(ninodes);
    if (inodestart != size) {
        check->error("%s expected inode start = %u, got %u\n", check->err, 
                    size, inodestart);
        check_size = false;
    }
    info->inodestart = inodestart;
    info->inodesize = ninode_blocks;
    size += ninode_blocks;

    /* Check bitmap. */
    uint bmapstart = xuint(sb->bmapstart);
    if (size != bmapstart) {
        check->error("%s expected bitmap start = %u, got %u\n", check->err, 
                    size, bmapstart);
        check_size = false;
    }
    info->bmapsize = BITMAP_BLOCKS(fssize);
    info->bmapstart = bmapstart;
    size += BITMAP_BLOCKS(fssize);

    /* Check file size. */
    uint datastart = size;
    info->datastart = datastart;
    info->datasize = xuint(sb->nblocks);
    size += xuint(sb->nblocks);
    if (fssize < size) {
        check_size = false;
        check->error("%s disk too small (%u blocks), should be at least %lu\n",
                    check->err, size, fssize);
    } else if (fssize > size) {
        check->warning("%s disk too large (%u blocks), expected %lu\n", 
                        check->warn, fssize, size);
    }
    info->fssize = size;

    return check_size;
}

int xv6_docheck(struct checker *check) noexcept {
    if (!check->bread || !check->bdata) {
        return 1;
    }

    if (!check->err) {
        check->err  = "\033[01;31merror:\033[0;m";
    }
    if (!check->warn) {
        check->warn = "\033[01;35mwarning:\033[0;m";
    }

#define onnull(pt, method) do {                                               \
    if (pt == nullptr) {                                                      \
        check->error("%s %s returned null. aborting\n", check->err, #method); \
        return 2;                                                             \
    }} while (0)

    struct superblock sb;
    void *const privat = check->privat;
    do {
        void *sb_buf = check->bread(privat, 0);
        defer(check->brelse(sb_buf));
        onnull(sb_buf, check->bread);
        sb = *(struct superblock *) sb_buf;
    } while (0);
    const uint magic = xuint(sb.magic);
    if (FSMAGIC != magic) {
        check->error("%s Incorrect magic number %x\n", check->err, magic);
        return 1;
    }

    /* Check size and start of each layer. */
    struct xv6_checker_info info;
    if (! xv6_check_sb(&sb, check, &info)) {
        /* Do not continue, for the layout of image is wrong. */
        check->error("%s possibly corrupted super block, aborting\n", check->err);
        return 1;
    }

    /* Check root directory. */
    struct dinode root;
    do {
        struct bufptr bp(check->bread(privat, info.inodestart), check);
        onnull(bp.buf_, check->bread);
        struct dinode *dino = reinterpret_cast<struct dinode *>(bp.data());
        root = *(dino + 1);
        if (dino->type != 0) {
            check->error("%s null inode should be zeroed", check->err);
            return 1;
        }
        ushort rt = __le16_to_cpu(root.type);
        if (rt != T_DIR) {
            check->error("%s root directory has incorrect type %u\n");
            return 1;
        }
    } while (0);
    /* Directory entry checker. */
    auto dir_check = [](uint dnum, struct dirent *de, 
            void *ctx) -> struct xv6_diter_action {
        auto *check = (checker *) ctx;
        xv6_diter_action ret = xv6_diter_action_init;
        ret.cont = 1;
        
        if (de->inum != 0) {
            check->warning("got %s\n", de->name);
        }
        return ret;
    };
    do {
        uint addrs[NDIRECT + 1];
        for (int i = 0; i <= NDIRECT; i++) {
            addrs[i] = __le32_to_cpu(root.addrs[i]);
        }
        struct xv6_inode_ctx rc = {
            .addrs = addrs,
            .size = __le32_to_cpu(root.size),
            .dirty = false,
        };

        if (xv6_dir_iterate(check, &rc, dir_check, check, 2, false) != 0) {
            check->error("%s iterating root directory failed.\n", check->err);
            return 1;
        }
    } while (0);

    /* All check passed. */
    return 0;
}
