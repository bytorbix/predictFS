#define FUSE_USE_VERSION (26)

#include <fuse.h>
#include "pfs.h"

int vfs_getattr(const char *path, struct stat *st);