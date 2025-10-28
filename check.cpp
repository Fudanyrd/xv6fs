
#include "check.h"

/* Buffer pointer manager; automatic free when dropped. */
struct bufptr {
    bufptr(void *buf, struct checker *ctx): buf_(buf), ctx_(ctx) {}
    
    /* Disallow copy. */
    bufptr(const bufptr &other) = delete;
    bufptr &operator=(const bufptr &other) = delete;

    ~bufptr() {
        if (this->buf_)
            ctx_->bfree(this->buf_);
    }

    void *data() {
        return ctx_->bdata(this->buf_);
    }

    void *buf_;
    struct checker *ctx_;
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


int xv6_docheck(struct checker *check) {
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
    }} while (0)

    struct bufptr sbptr(check->bread(0), check);
    onnull(sbptr.buf_, check->bread);
    struct superblock *sb = (struct superblock *) sbptr.data();
    const uint magic = xuint(sb->magic);
    if (FSMAGIC != magic) {
        check->error("%s Incorrect magic number %x\n", check->err, magic);
        return 1;
    }

    /* All check passed. */
    return 0;
}
