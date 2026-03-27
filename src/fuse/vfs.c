#include "vfs.h"
#include "fs.h"
#include "dir.h"
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
        if (flag == -1) return -ENOENT;
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

int vfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    // get the dir inode
    ssize_t dir_inode_num = fs_lookup(pfs->fs, path);
    if (dir_inode_num < 0) {
        return -ENOENT;
    }
    Inode *inode = fs_read_inode(pfs->fs, (size_t)dir_inode_num);
    if (inode == NULL) {
        return -EIO;
    }
    // confirming it's a directory
    if (inode->valid == INODE_DIR) {
        for (size_t i = 0; i < inode->size; i+= sizeof(DirEntry)) {
            DirEntry entry;
            fs_read(pfs->fs, dir_inode_num, (char *)&entry, sizeof(DirEntry), i);
            if (entry.inode_number != UINT32_MAX) {
                filler(buf, entry.name, NULL, 0);
            }
        }
        free(inode);
        return 0;
    }
    else {
        // not a directory
        free(inode);
        return -ENOTDIR;
    }
}

int vfs_open(const char *path, struct fuse_file_info *fi) {
    ssize_t inode = fs_lookup(pfs->fs, path);
    if (inode == -1) return -ENOENT;
    return 0;
}

int vfs_read(const char *path, char *buffer, size_t length, off_t offset, struct fuse_file_info *fi) {
    ssize_t inode = fs_lookup(pfs->fs, path);
    if (inode < 0) {
        return -ENOENT;
    }
    ssize_t bytes_red = fs_read(pfs->fs, inode, buffer, length, (size_t)offset);
    if (bytes_red < 0) return -EIO;
    return bytes_red;
}

int vfs_write(const char *path, const char *buf, size_t length, off_t offset, struct fuse_file_info *fi) {
    ssize_t inode = fs_lookup(pfs->fs, path);
    if (inode < 0) {
        return -ENOENT;
    }
    ssize_t bytes_written = pfs_write(pfs, inode, buf, length, (size_t)offset);
    if (bytes_written < 0) return -EIO;
    return bytes_written;
}

int vfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    ssize_t inode = pfs_create(pfs, path);
    if (inode < 0) {
        return -ENOENT;
    }
    return 0;
}

int vfs_unlink(const char *path) {
    ssize_t inode = fs_lookup(pfs->fs, path);
    if (inode < 0) {
        return -ENOENT;
    }
    ssize_t flag = pfs_remove(pfs, inode);
    if (flag < 0) return -EIO;
    char *parentdir = extract_parentdir(path);
    ssize_t dir_inode = fs_lookup(pfs->fs, parentdir);
    if (dir_inode < 0) {
        free(parentdir);
        return -ENOENT;
    }
    dir_remove(pfs->fs, dir_inode, extract_filename(path));
    free(parentdir);
    return 0;
}

int vfs_mkdir(const char *path, mode_t mode) {
    ssize_t inode = dir_create(pfs->fs);
    if (inode < 0) {
        return -ENOENT;
    }
    char *parentdir = extract_parentdir(path);
    ssize_t dir_inode = fs_lookup(pfs->fs, parentdir);
    if (dir_inode < 0) {
        free(parentdir);
        return -ENOENT;
    }
    int flag = dir_add(pfs->fs, dir_inode, extract_filename(path), inode);
    if (flag < 0) {
        free(parentdir);
        return -EIO;
    }
    free(parentdir);
    return 0;

}

int vfs_rmdir(const char *path) {
    ssize_t inode = fs_lookup(pfs->fs, path);
    if (inode < 0) {
        return -ENOENT;
    }
    Inode *inode_dir = fs_read_inode(pfs->fs, inode);
    if (inode_dir->size > 0) {
        free(inode_dir);
        return -ENOTEMPTY;
    }
    ssize_t flag = fs_remove(pfs->fs, inode);
    if (flag < 0) {
        free(inode_dir);
        return -EIO;
    }
    
    char *parentdir = extract_parentdir(path);
    ssize_t dir_inode = fs_lookup(pfs->fs, parentdir);
    if (dir_inode < 0) {
        free(parentdir);
        return -ENOENT;
    }
    dir_remove(pfs->fs, dir_inode, extract_filename(path));
    free(parentdir);
    free(inode_dir);
    return 0;
}

int vfs_truncate(const char *path, off_t size) {
    ssize_t inode = fs_lookup(pfs->fs, path);
    if (inode < 0) {
        return -ENOENT;
    }
    bool flag =fs_truncate(pfs->fs, inode);
    if (!flag) return -EIO;
    return 0;
}

// registered ops
static struct fuse_operations ops = {
    .getattr = vfs_getattr,
    .readdir = vfs_readdir,
    .open = vfs_open,
    .read = vfs_read,
    .write = vfs_write,
    .create = vfs_create,
    .unlink = vfs_unlink,
    .mkdir = vfs_mkdir,
    .rmdir = vfs_rmdir,
    .truncate = vfs_truncate
};

int main(int argc, char *argv[]) 
{
    Disk *disk = disk_open("disk.img", 1000);
    pfs_format(disk);
    pfs = calloc(1, sizeof(pFileSystem));
    pfs_mount(pfs, disk);
    return fuse_main(argc, argv, &ops, NULL);
}

