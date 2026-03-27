#define FUSE_USE_VERSION (26)

#include <fuse.h>
#include "pfs.h"
#include "utils.h"

int vfs_getattr(const char *path, struct stat *st);
int vfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi);
int vfs_open(const char *path, struct fuse_file_info *fi);
int vfs_read(const char *path, char *buffer, size_t length, off_t offset, struct fuse_file_info *fi);
int vfs_write(const char *path, const char *buf, size_t length, off_t offset, struct fuse_file_info *fi);
int vfs_create(const char *path, mode_t mode, struct fuse_file_info *fi);
int vfs_unlink(const char *path);
int vfs_mkdir(const char *path, mode_t mode);
int vfs_rmdir(const char *path);
int vfs_truncate(const char *path, off_t size);