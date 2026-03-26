#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disk.h"
#include "fs.h"
#include "pfs.h"

int main() {

    Disk *disk = disk_open("disk.img", 1000);
    pfs_format(disk);
    pFileSystem pfs = {0};
    pfs_mount(&pfs, disk);

    // create a file
    ssize_t inode = pfs_create(&pfs, "/test.log");

    // first write
    char buf[600];
    memset(buf, 'A', sizeof(buf));
    pfs_write(&pfs, inode, buf, 100, 0);

    // second write
    pfs_write(&pfs, inode, buf, 400, 100);

    // remove and update stats
    pfs_remove(&pfs, inode);

    // print bucket 0 stats
    ExtensionEntry *entry = find_entry(&pfs, "log");
    if (entry) {
        BucketStats *bucket = &entry->buckets[0];
        printf("bucket[0] count=%u tendency=%.2f mean_final=%.0f growing_count=%u mean_ratio=%.2f confidence=%.2f\n",
            bucket->count, bucket->tendency, bucket->mean_final_size,
            bucket->growing_count, bucket->mean_ratio, pfs_confidence(bucket));
    }

    // file that doesn't grow
    // first write 500 bytes 
    ssize_t inode2 = pfs_create(&pfs, "/static.log");
    pfs_write(&pfs, inode2, buf, 500, 0);
    pfs_remove(&pfs, inode2);

    // tendency should drop to 0.50 
    entry = find_entry(&pfs, "log");
    if (entry) {
        BucketStats *bucket = &entry->buckets[0];
        printf("bucket[0] count=%u tendency=%.2f mean_final=%.0f\n",
            bucket->count, bucket->tendency, bucket->mean_final_size);
    }

    // bucket 1 routing 
    char *big = calloc(6000, 1);  // should land in bucket 1
    ssize_t inode3 = pfs_create(&pfs, "/big.log");
    pfs_write(&pfs, inode3, big, 6000, 0);
    pfs_remove(&pfs, inode3);
    free(big);

    entry = find_entry(&pfs, "log");
    if (entry) {
        printf("bucket[1] count=%u tendency=%.2f mean_final=%.0f\n",
            entry->buckets[1].count, entry->buckets[1].tendency, entry->buckets[1].mean_final_size);
    }

    // persistence across remount 
    pfs_unmount(&pfs);

    Disk *disk2 = disk_open("disk.img", 1000);
    pFileSystem pfs2 = {0};
    pfs_mount(&pfs2, disk2);

    entry = find_entry(&pfs2, "log");
    if (entry) {
        printf("after remount — bucket[0] count=%u tendency=%.2f\n",
            entry->buckets[0].count, entry->buckets[0].tendency);
    }

    // pre allocation trigger
    // train with 60 log files
    char small[500];
    memset(small, 'B', sizeof(small));
    for (int i = 0; i < 60; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/train%d.log", i);
        ssize_t t = pfs_create(&pfs2, path);
        pfs_write(&pfs2, t, small, 100, 0);
        pfs_write(&pfs2, t, small, 400, 100);
        pfs_remove(&pfs2, t);
    }

    // print confidence after training
    ExtensionEntry *entry2 = find_entry(&pfs2, "log");
    if (entry2) {
        BucketStats *b = &entry2->buckets[0];
        printf("after training — count=%u tendency=%.2f mean_ratio=%.2f confidence=%.2f\n",
            b->count, b->tendency, b->mean_ratio, pfs_confidence(b));
    }

    // now create a file and check extent count after writes
    ssize_t pred = pfs_create(&pfs2, "/predicted.log");
    pfs_write(&pfs2, pred, small, 100, 0);

    Inode *inode_check = fs_read_inode(pfs2.fs, pred);
    if (inode_check) {
        printf("predicted file extent_count=%u\n", inode_check->extent_count);
        free(inode_check);
    }
    pfs_remove(&pfs2, pred);

    pfs_unmount(&pfs2);


    return 0;
}
