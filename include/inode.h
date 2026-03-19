#pragma once
#include <stdint.h>

// Extent declaration
typedef struct Extent Extent;
struct Extent {
    uint32_t start; // First physical Block
    uint32_t length; // Amount of contiguous allocated blocks 
};

#define EXTENTS_PER_BLOCK (BLOCK_SIZE / sizeof(Extent))
#define EXTENTS_PER_INODE (3)


// Inode Status
#define INODE_FREE 0
#define INODE_FILE 1
#define INODE_DIR 2


// struct

typedef struct Inode Inode;
struct Inode
{
    uint32_t valid;                      // Status: 0 Indicates Free/Deleted Slot, 
                                         // 1 Indicatesa an Allocated File Inode 
                                         // 2 Indicates a Directory Inode
    uint32_t size;                       // File size in bytes.
    uint32_t extent_count;               // Count of allocated extents
    Extent extents[EXTENTS_PER_INODE];   // First Direct Extents
    uint32_t extent_block;               // Extents Block Physical Block
    // FUSE related fields needed (perms, time)
    
};

