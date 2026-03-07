#include "vfs.h"
#include <string.h>
#include <errno.h>

static pFileSystem *pfs = NULL;


int vfs_getattr(const char *path, struct stat *st) 
{
    memset(st, 0, sizeof(struct stat));
    if (strcmp(path, "/") == 0) 
    {
        st->st_mode = S_IFDIR | 0755; // set mode as dir and root perms
        st->st_nlink = 2; // file is dir
    }
    else 
    {
        ssize_t flag = fs_lookup(pfs->fs, path);
        if (flag == -1) 
        {
            return -ENOENT;
        }
        Inode* inode = fs_read_inode(pfs->fs, flag);
        if (inode == NULL) 
        {
            return -EIO;
        }
        if (inode->valid == INODE_DIR) 
        {
            st->st_mode = S_IFDIR | 0755;
            st->st_nlink = 2;
        } else 
        {
            st->st_mode = S_IFREG | 0644;
            st->st_nlink = 1;
        }
        st->st_size = inode->size;
        free(inode);

    }
    return 0;
}

static struct fuse_operations ops = {
    .getattr = vfs_getattr
};

int main(int argc, char *argv[]) 
{
    Disk *disk = disk_open("disk.img", 1000);
    pfs_format(disk);
    pfs = calloc(1, sizeof(pFileSystem));
    pfs_mount(pfs, disk);
    return fuse_main(argc, argv, &ops, NULL);
}

