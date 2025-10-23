#include <linux/fs.h>

static const struct file_operations xv6_file_ops = {
    .owner = THIS_MODULE,
};

