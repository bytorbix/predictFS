#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disk.h"
#include "fs.h"

void print_inode_extents(FileSystem *fs, ssize_t inode_number)
{
    Inode *inode = fs_read_inode(fs, inode_number);
    if (!inode) { printf("  [inode read failed]\n"); return; }

    printf("  inode %zd: size=%u, extent_count=%u\n",
           inode_number, inode->size, inode->extent_count);

    uint32_t inline_count = inode->extent_count < EXTENTS_PER_INODE
                            ? inode->extent_count : EXTENTS_PER_INODE;
    for (uint32_t i = 0; i < inline_count; i++) {
        printf("    [inline %u] start=%-4u length=%u\n",
               i, inode->extents[i].start, inode->extents[i].length);
    }

    if (inode->extent_count > EXTENTS_PER_INODE && inode->extent_block != 0) {
        printf("    [extent_block] phys=%u\n", inode->extent_block);
        Block buf;
        uint32_t inode_block_idx = 1 + (inode_number / INODES_PER_BLOCK);
        // re-read from disk to get extent block
        if (disk_read(fs->disk, inode->extent_block, buf.data) == 0) {
            uint32_t overflow = inode->extent_count - EXTENTS_PER_INODE;
            for (uint32_t i = 0; i < overflow; i++) {
                printf("    [overflow %u] start=%-4u length=%u\n",
                       i, buf.extents[i].start, buf.extents[i].length);
            }
        }
        (void)inode_block_idx;
    }

    free(inode);
}

void print_passed(const char* message) 
{ 
    printf("[OK]:   %s\n", message); 
}

void print_failed(const char* message) { 
    printf("[FAIL]: %s\n", message); 
}

int main() {
    Disk *disk = disk_open("disk.img", 1000);
    if (!disk) { print_failed("disk_open"); return 1; }

    if (!fs_format(disk)) { print_failed("fs_format"); return 1; }
    print_passed("fs_format");

    FileSystem fs = {0};
    if (!fs_mount(&fs, disk)) { print_failed("fs_mount"); return 1; }
    print_passed("fs_mount");

    // --- Test 1: create ---
    ssize_t inode = fs_create(&fs);
    if (inode < 0) { print_failed("fs_create"); return 1; }
    print_passed("fs_create");

    // --- Test 2: write + read (small) ---
    char *msg = "Hello, extents!";
    size_t msg_len = strlen(msg);
    if (fs_write(&fs, inode, msg, msg_len, 0) != (ssize_t)msg_len) {
        print_failed("fs_write small");
    } else {
        print_passed("fs_write small");
    }

    char rbuf[64] = {0};
    if (fs_read(&fs, inode, rbuf, msg_len, 0) != (ssize_t)msg_len || strcmp(rbuf, msg) != 0) {
        print_failed("fs_read small");
    } else {
        print_passed("fs_read small");
    }

    // --- Test 3: stat ---
    ssize_t size = fs_stat(&fs, inode);
    if (size != (ssize_t)msg_len) {
        print_failed("fs_stat");
    } else {
        print_passed("fs_stat");
    }

    // --- Test 4: write + read (multi-block, 3 blocks) ---
    ssize_t inode2 = fs_create(&fs);
    size_t large_size = BLOCK_SIZE * 3;
    char *wbuf = malloc(large_size);
    char *rbuf2 = malloc(large_size);
    memset(wbuf, 0xAB, large_size);

    if (fs_write(&fs, inode2, wbuf, large_size, 0) != (ssize_t)large_size) {
        print_failed("fs_write multi-block");
    } else {
        print_passed("fs_write multi-block");
    }

    if (fs_read(&fs, inode2, rbuf2, large_size, 0) != (ssize_t)large_size ||
        memcmp(wbuf, rbuf2, large_size) != 0) {
        print_failed("fs_read multi-block");
    } else {
        print_passed("fs_read multi-block");
    }

    free(wbuf);
    free(rbuf2);

    // --- Test 5: remove ---
    if (!fs_remove(&fs, inode)) {
        print_failed("fs_remove");
    } else {
        print_passed("fs_remove");
    }

    // stat on removed inode should return -1
    if (fs_stat(&fs, inode) != -1) {
        print_failed("fs_stat on removed inode should return -1");
    } else {
        print_passed("fs_stat on removed inode returns -1");
    }

    // --- Test 7: extent fragmentation + overflow ---
    // Interleave 1-block writes between two files to force non-contiguous extents.
    // EXTENTS_PER_INODE=3, so after 4 fragmented writes file_a spills to the extent block.
    {
        ssize_t file_a = fs_create(&fs);
        ssize_t file_b = fs_create(&fs);

        char *blk_w = malloc(BLOCK_SIZE);
        char *blk_r = malloc(BLOCK_SIZE);
        int ok = 1;

        for (int i = 0; i < 5; i++) {
            // write block i to file_a with pattern 0xA0+i
            memset(blk_w, 0xA0 + i, BLOCK_SIZE);
            if (fs_write(&fs, file_a, blk_w, BLOCK_SIZE, i * BLOCK_SIZE) != (ssize_t)BLOCK_SIZE)
                { ok = 0; break; }

            // write a filler block to file_b to break contiguity
            memset(blk_w, 0xFF, BLOCK_SIZE);
            if (fs_write(&fs, file_b, blk_w, BLOCK_SIZE, i * BLOCK_SIZE) != (ssize_t)BLOCK_SIZE)
                { ok = 0; break; }

            printf("  -- after write %d --\n", i);
            print_inode_extents(&fs, file_a);
        }

        if (!ok) {
            print_failed("extent fragmentation: write");
        } else {
            print_passed("extent fragmentation: write");
        }

        // verify each block of file_a has the correct pattern
        ok = 1;
        for (int i = 0; i < 5; i++) {
            memset(blk_r, 0, BLOCK_SIZE);
            if (fs_read(&fs, file_a, blk_r, BLOCK_SIZE, i * BLOCK_SIZE) != (ssize_t)BLOCK_SIZE) {
                ok = 0; break;
            }
            // check first and last byte of the block
            if ((unsigned char)blk_r[0] != (unsigned char)(0xA0 + i) ||
                (unsigned char)blk_r[BLOCK_SIZE - 1] != (unsigned char)(0xA0 + i)) {
                ok = 0; break;
            }
        }
        if (!ok) {
            print_failed("extent fragmentation: read verify");
        } else {
            print_passed("extent fragmentation: read verify (inline + overflow extents)");
        }

        fs_remove(&fs, file_a);
        fs_remove(&fs, file_b);
        free(blk_w);
        free(blk_r);
    }

    // --- Test 6: unmount + remount (persistence) ---
    fs_unmount(&fs);  // also closes disk
    print_passed("fs_unmount");

    Disk *disk2 = disk_open("disk.img", 1000);
    if (!disk2) { print_failed("disk_open for remount"); return 1; }

    FileSystem fs2 = {0};
    if (!fs_mount(&fs2, disk2)) { print_failed("fs_mount after unmount"); return 1; }
    print_passed("fs_mount after unmount");

    char rbuf3[64] = {0};
    if (fs_read(&fs2, inode2, rbuf3, 4, 0) < 0 || (unsigned char)rbuf3[0] != 0xAB) {
        print_failed("fs_read after remount");
    } else {
        print_passed("fs_read after remount");
    }

    fs_unmount(&fs2);  // also closes disk2
    return 0;
}
