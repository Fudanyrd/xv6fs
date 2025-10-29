/*
 * Xv6 filesystem image verifier. It scans the image for potential 
 * errors, possibly as a result of bug(s) in my kernel module.
 */
#include "check.h"

#include <stdarg.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void usage(void) {
    printf("usage: checker [xv6 fs image]\n");
}

static void xv6_message(const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
}

__attribute__((noreturn))
static void panic(const char *fmt, ...) {
    va_list va;
    va_start(va, fmt);
    vfprintf(stderr, fmt, va);
    va_end(va);
    abort();
}

static size_t fsize;
static size_t mapsize;

struct mapptr {
    mapptr(void *pt, size_t size): ptr_(pt), map_size_(size) {}
    ~mapptr() {
        munmap(this->ptr_, this->map_size_);
    }

    void *ptr_;
    size_t map_size_;
};

static void *checker_bread(void *privat, uint block) {
    if (block >= fsize / BSIZE) {
        /* Attempting out-of-bound access. */
        return nullptr;
    }
    uintptr_t addr = (uintptr_t) privat;
    return (void *)(addr + (block * BSIZE));
}

static void *checker_bdata(void *buf) {
    return buf;
}

static void checker_brelse(void *) {}

int main(int argc, char **argv) {
    if (argc != 2) {
        usage();
        return 1;
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        perror(argv[1]);
        return 1;
    }
    do {
        struct stat st;
        if (fstat(fd, &st) < 0) {
            perror("stat");
            return 1;
        }
        fsize = st.st_size;
        mapsize = fsize + 4095;
        mapsize = mapsize & (~((size_t) 4095));
    } while (0);
    void *fmap = mmap(NULL, mapsize, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (fmap == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    mapptr dummy(fmap, mapsize); /* Create a manager to do the munmap. */

    struct checker check;
    check.err = nullptr;
    check.privat = fmap;
    check.warn = nullptr;
    check.warning  = check.error = xv6_message;
    check.bdata = checker_bdata;
    check.bread = checker_bread;
    check.brelse = checker_brelse;
    check.panic = panic;

    return xv6_docheck(&check);
}
