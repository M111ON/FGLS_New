#define FUSE_USE_VERSION 26
#define _FILE_OFFSET_BITS 64

#include <fuse/fuse.h>
#include <stdio.h>

static int test_getattr(const char *path, struct stat *st) {
    (void)path; (void)st;
    return -ENOENT;
}

static struct fuse_operations test_ops = {
    .getattr = test_getattr,
};

int main(int argc, char *argv[]) {
    return fuse_main(argc, argv, &test_ops, NULL);
}