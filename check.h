#ifndef _CHECK_H
#define _CHECK_H 1

#include "fs.h"

#ifdef __cplusplus
extern "C" {
#endif /* C++ */

struct checker {
    void *privat;  /**< Private struct needed to read blocks
                     Don't ask me why I misspelled it 😭 */
    void *(* bread)(void *, uint); /**< Read a disk block, return the buffer struct. */
    void *(* bdata)(void *); /**< Given the buffer, get its internal data. */
    void (*brelse)(void *); /**< Release an buffer. */
    int (* balloc)(void *, uint *); /**< same as xv6_balloc(). */ 
    int (* bflush)(void *sb, void *buf); /**< Sync dirty buffer. */

    const char *warn; /**< Prefix of warning message. */
    void (*warning)(const char *fmt, ...);

    const char *err; /**< Prefix of error message. */
    void (*error)(const char *fmt, ...);

    void (*panic)(const char *fmt, ...) __attribute__((noreturn));
};

/** Run the xv6 filesystem checker. */
extern int xv6_docheck(struct checker *check) __attribute__((nothrow));

#ifdef __cplusplus
}
/* extern "C" */

/* Buffer pointer manager; automatic free when dropped. */
struct bufptr {
    bufptr(void *buf, struct checker *ctx): buf_(buf), ctx_(ctx) {}
    
    /* Disallow copy. */
    bufptr(const bufptr &other) = delete;
    bufptr &operator=(const bufptr &other) = delete;

    ~bufptr() {
        if (this->buf_)
            ctx_->brelse(this->buf_);
    }

    void *data() {
        return ctx_->bdata(this->buf_);
    }

    void *buf_;
    struct checker *ctx_;
};

#endif /* C++ */

#endif /* _CHECK_H 1 */
