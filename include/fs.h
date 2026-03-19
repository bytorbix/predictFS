/* File System */

#pragma once


#include "disk.h"
#include "bitmap.h"
#include "inode.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h> 

// File System Constants
#define MAGIC_NUMBER (0xf0f03410)
#define INODES_PER_BLOCK (BLOCK_SIZE / sizeof(Inode))

// File System Structure

typedef struct SuperBlock SuperBlock;
struct SuperBlock {
    uint32_t magic_number;  // File system type identifier
    uint32_t blocks;        // Total number of blocks in the file system
    uint32_t inode_blocks;  // Total number of blocks reserved for the Inode Table
    uint32_t inodes;        // Total number of Inode structures
    uint32_t bitmap_blocks;  // Total amount of bitmap blocks
};

typedef union Block Block;
union Block {
    // Block Roles: A single block can only serve ONE of these purposes at a time.
    
    SuperBlock super;                      // File System Metadata: Contains the SuperBlock structure (Block 0).
    Inode inodes[INODES_PER_BLOCK];        // Inode Table Block: Stores an array of 128 Inode structures (metadata for files).
    Extent extents[EXTENTS_PER_BLOCK];     // Extents Block: An array of extents stored on a block
    char data[BLOCK_SIZE];                 // Data Block: Raw storage for file content.

};


typedef struct FileSystem FileSystem;
struct FileSystem {
    Disk *disk;             // Instance of the emulated Disk
    Bitmap *bitmap;      // Array of free blocks, (In-Memory Bitmap Cache)
    uint32_t *ibitmap;     // Array of free blocks (In-Memory Inodes Bitmap Cache)
    SuperBlock *meta_data;  // Meta data of the file system
};


/* File System Functions Prototypes (Declarations) */

void fs_debug(FileSystem *fs);
bool fs_format(Disk *disk);
bool fs_mount(FileSystem *fs, Disk *disk);
void fs_unmount(FileSystem *fs);
ssize_t fs_create(FileSystem *fs);
bool fs_remove(FileSystem *fs, size_t inode_number);
ssize_t fs_stat(FileSystem *fs, size_t inode_number);
ssize_t fs_read(FileSystem *fs, size_t inode_number, char *data, size_t length, size_t offset);
ssize_t fs_write(FileSystem *fs, size_t inode_number, char *data, size_t length, size_t offset);
Extent fs_allocate(FileSystem *fs, size_t blocks_to_reserve, uint32_t extent_block);
ssize_t fs_lookup(FileSystem *fs, const char *path);
Inode* fs_read_inode(FileSystem *fs, size_t inode_number);
uint32_t extent_lookup(FileSystem *fs, Inode *inode, uint32_t logical_block);

bool extent_add(FileSystem *fs, Inode *inode, uint32_t start, uint32_t length);