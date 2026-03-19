#include "fs.h"
#include "disk.h"
#include "dir.h"
#include "bitmap.h"
#include "utils.h"
#include "inode.h"
#include <stdio.h> 
#include <string.h> 
#include <stdlib.h> 
#include <math.h>



/* File System Functions Definitions */
void fs_debug(FileSystem *fs) {
    if (fs->disk == NULL || fs == NULL) {
        perror("fs_debug: Error disk is invalid");
        return;
    }
    if (!(fs->disk->mounted)) { 
        fprintf(stderr, "fs_debug: Error disk is not mounted, aborting...\n");
        return;
    }

    printf("SuperBlock\n");
    SuperBlock *super = fs->meta_data;
    bool magic_number = (super->magic_number == 0xf0f03410);
    printf("\tMagic Number is %s\n", (magic_number) ? "Valid" : "Invalid");
    printf("\tTotal Blocks: %d\n", super->blocks);
    printf("\tInode Blocks: %d\n", super->inode_blocks);
    printf("\tTotal Inodes: %d\n", super->inodes);

    printf("Bitmap\n");
    
}



bool fs_format(Disk *disk) 
{
    if (disk == NULL) {
        perror("fs_format: Error disk is invalid");
        return false;
    }
    if (disk->mounted) { 
        fprintf(stderr, "fs_format: Error disk is mounted, aborting to prevent data loss\n");
        return false;
    }

    // Initializing the super block
    SuperBlock superblock; 
    superblock.magic_number = MAGIC_NUMBER;
    superblock.blocks = (uint32_t)disk->blocks;
    superblock.bitmap_blocks = (superblock.blocks + BITS_PER_BITMAP_BLOCK - 1) / BITS_PER_BITMAP_BLOCK; // Calculation of the bitmap blocks needed for the entire disk

    // Inodes
    double percent_blocks = (double)superblock.blocks * 0.10;   
    superblock.inode_blocks = (uint32_t)ceil(percent_blocks);
    superblock.inodes = superblock.inode_blocks * INODES_PER_BLOCK;

    
    // Capacity check
    if ((1 /*Superblock*/) + superblock.inode_blocks + superblock.bitmap_blocks + 1 >= superblock.blocks) {
        fprintf(stderr, "fs_format: Error metadata blocks amount (%u) exceeds disk capacity (%u)\n", 
                1 + superblock.inode_blocks+superblock.bitmap_blocks + 1, superblock.blocks);
        return false;
    }

    // Initializing The Block Buffer
    Block block_buffer;
    for (int i = 0; i < BLOCK_SIZE; i++) {
        block_buffer.data[i] = 0;
    }
    block_buffer.super = superblock; // Copy the initialized superblock into the union
    

    // attempt to write the metadata into the magic block
    if (disk_write(disk, 0, block_buffer.data) < 0) {
        perror("fs_format: Failed to write SuperBlock to disk");
        return false;
    }


    // format the bitmaps blocks
    if (!format_bitmap(disk, superblock.inode_blocks, superblock.bitmap_blocks)) {
        perror("fs_format: Failed to format bitmap");
        return false;
    }
    
    // cleaning the block_buffer for Re-use
    memset(block_buffer.data, 0, BLOCK_SIZE);

    // formatting the pfs block
    if (disk_write(disk, superblock.inode_blocks + superblock.bitmap_blocks + 1, block_buffer.data) < 0) {
        perror("fs_format: Failed to write to disk");
        return false;
    }

    // Clean the inode table
    for (uint32_t i = 1; i <= superblock.inode_blocks; i++) {
        if (disk_write(disk, i, block_buffer.data) < 0) {
            perror("fs_format: Failed to clear inode table blocks");
            return false;
        }
    }

    // Set The Inode 0 as root dir
    Block buffer;
    memset(buffer.data, 0, BLOCK_SIZE);
    Inode *target = &buffer.inodes[0];
    target->valid = INODE_DIR;
    target->size = 0;

    if (disk_write(disk, 1, buffer.data) < 0) {
        perror("fs_format: Failed to write block to disk");
        return false;
    }
    // success
    return true;
}

bool fs_mount(FileSystem *fs, Disk *disk) {
    if (fs == NULL || disk == NULL) {
        perror("fs_mount: Error fs or disk is invalid (NULL)"); 
        return false;
    }
    if (disk->mounted) { 
        fprintf(stderr, "fs_mount: Error disk is already mounted, cannot mount it\n");
        return false;
    }

    Block block_buffer;
    if (disk_read(disk, 0, block_buffer.data)< 0) {
        perror("fs_mount: Failed to read super block from disk");
        return false;
    }

    SuperBlock superblock = block_buffer.super;
    if (superblock.magic_number != MAGIC_NUMBER) {
        fprintf(stderr, 
                "fs_mount: Error Disk magic number (0x%x) is invalid. Expected (0x%x).\n"
                "The disk is either unformatted or corrupted.\n",
                superblock.magic_number, MAGIC_NUMBER);
        return false;
    }
    if (superblock.blocks != disk->blocks) {
        fprintf(stderr, "fs_mount: Error Super block amount of blocks (%u) mismatch the disk capacity (%zu), aborting...\n",
                superblock.blocks, disk->blocks);
        return false;
    }

    fs->disk = disk;
    // Allocate memory for the SuperBlock metadata and copy it
    fs->meta_data = (SuperBlock *)malloc(sizeof(SuperBlock));
    if (fs->meta_data == NULL) return false;
    *(fs->meta_data) = superblock; // Copy the structure contents

    uint32_t total_inodes = fs->meta_data->inodes;
    uint32_t meta_data_blocks = fs->meta_data->inode_blocks + fs->meta_data->bitmap_blocks + 2;
    

    // Bitmap — allocate full blocks so disk_read won't overflow the buffer
    uint32_t bitmap_words = fs->meta_data->bitmap_blocks * (BLOCK_SIZE / sizeof(uint32_t));

    fs->bitmap = calloc(1, sizeof(Bitmap));
    if (fs->bitmap == NULL) {
        perror("fs_mount: Failed to allocate bitmap struct");
        free(fs->meta_data);
        return false;
    }
    fs->bitmap->bits = calloc(bitmap_words, sizeof(uint32_t));
    if (fs->bitmap->bits == NULL) {
        perror("fs_mount: Failed to allocate bitmap array");
        free(fs->bitmap);
        free(fs->meta_data);
        return false;
    }

    bool bitmap_loaded_valid = load_bitmap(fs);
    if (bitmap_loaded_valid) {
        if (get_bit(fs->bitmap->bits, 0) == 0) {
            bitmap_loaded_valid = false; 
        }
    }

    // Try to load from disk. 
    if (!bitmap_loaded_valid) 
    {
        memset(fs->bitmap->bits, 0, bitmap_words * sizeof(uint32_t));

        // Mark Metadata blocks as allocated
        for(uint32_t k=0; k < meta_data_blocks; k++) {
            set_bit(fs->bitmap->bits, k, 1);
        }

        // Scan Inodes to mark data blocks
        // Loop through Inode Blocks (starts at block 1)
        for (size_t i = 1; i <= fs->meta_data->inode_blocks; i++)
        {
            Block inode_buffer;
            if (disk_read(fs->disk, i, inode_buffer.data) < 0) {
                free(fs->meta_data);
                free(fs->bitmap->bits);
                free(fs->bitmap);
                return false;
            }

            for (uint32_t j = 0; j < INODES_PER_BLOCK; j++) 
            {
                Inode *inode = &inode_buffer.inodes[j];
                if (!inode->valid) continue; // check if inode is valid, if not we skip
                // Check Extents
                for (uint32_t e = 0; e < inode->extent_count && e < EXTENTS_PER_INODE; e++)
                {
                    for (uint32_t b = 0; b < inode->extents[e].length; b++) {
                        set_bit(fs->bitmap->bits, inode->extents[e].start + b, 1);
                    }
                }
                
                // iterate the extents block
                if (inode->extent_block != 0) 
                {
                    Block extents_buf;
                    if (disk_read(fs->disk, inode->extent_block, extents_buf.data) < 0) {
                        perror("fs_mount: Error reading from disk has failed");
                        free(fs->meta_data);
                        free(fs->bitmap->bits);
                        free(fs->bitmap);
                        return false;
                    }

                    for (size_t k = 0; k < EXTENTS_PER_BLOCK; k++)
                    {
                        Extent *extent = &extents_buf.extents[k];
                        for (uint32_t e = 0; e < extent->length; e++) {
                            set_bit(fs->bitmap->bits, extent->start + e, 1);
                        }
                    }
                }
            }
        }
    }

    // Inodes Bitmap
    // ibitmap in it's initial form iterates through all the inodes and read if valid or not
    uint32_t ibitmap_words = (total_inodes + BITS_PER_WORD - 1) / BITS_PER_WORD;
    uint32_t *ibitmap = calloc(ibitmap_words, sizeof(uint32_t));
    if (ibitmap == NULL) {
        perror("fs_mount: Failed to allocate memory for ibitmap array");
        free(fs->meta_data);
        free(fs->bitmap->bits);
        free(fs->bitmap);
        return false;

    }
    fs->ibitmap = ibitmap;

    uint32_t current_inode_id = 0;
    for (size_t i = 1; i <= fs->meta_data->inode_blocks; i++)
    {
        Block inode_buffer;
        if (disk_read(fs->disk, i, inode_buffer.data) < 0) {
            // Error handling (free memory and return false)
            free(fs->meta_data);
            free(fs->bitmap->bits);
            free(fs->bitmap);
            free(fs->ibitmap);
            return false;
        }

        for (size_t j = 0; j < INODES_PER_BLOCK; j++)
        {
            if (current_inode_id >= total_inodes) break;
            if (inode_buffer.inodes[j].valid) {
                set_bit(fs->ibitmap, current_inode_id, 1);
            }
            current_inode_id++;
        }
        
    }
    
    
    disk->mounted=true;
    return true;
}

void fs_unmount(FileSystem *fs) {
    // Error Checks
    if (fs == NULL) {
        perror("fs_unmount: Error file system pointer is NULL");
        return;
    }
    
    // Check disk pointer 
    if (fs->disk == NULL) {
        perror("fs_unmount: Warning, disk pointer is NULL but continuing cleanup");
    } else if (!fs->disk->mounted) { 
        fprintf(stderr, "fs_unmount: Warning disk is already unmounted\n");
        // Continue cleanup in case memory was still allocated
    }

    // Flush dirty bitmap before freeing meta_data (save_bitmap needs it)
    if (fs->bitmap != NULL && fs->bitmap->dirty) {
        save_bitmap(fs);
    }

    // Memory Cleanup
    if (fs->meta_data != NULL) {
        free(fs->meta_data);
        fs->meta_data = NULL;
    }
    if (fs->bitmap != NULL)
    {
        free(fs->bitmap->bits);
        free(fs->bitmap);
        fs->bitmap = NULL;
    }

    if (fs->ibitmap != NULL) {
        free(fs->ibitmap);
        fs->ibitmap = NULL;
    }

    if (fs->disk != NULL) {
        disk_close(fs->disk); 
        fs->disk = NULL;
    }
}


// Allocates contiguous disk blocks. Tries desired_extent_block first to enable
// extent merging, falls back to best-fit scan. Returns {0,0} on failure.
Extent fs_allocate(FileSystem *fs, size_t blocks_to_reserve, uint32_t desired_extent_block) {
    if (fs == NULL || fs->meta_data == NULL || fs->bitmap == NULL || fs->disk == NULL) {
        perror("fs_allocate: Error fs, metadata, bitmap, or disk is invalid (NULL)");
        return (Extent){0, 0};
    }

    if (!(fs->disk->mounted)) {
        fprintf(stderr, "fs_allocate: Error disk is not mounted\n");
        return (Extent){0, 0};
    }

    uint32_t *bitmap      = fs->bitmap->bits;
    uint32_t total_blocks = fs->meta_data->blocks;
    size_t meta_blocks    = 2 + fs->meta_data->inode_blocks + fs->meta_data->bitmap_blocks;

    if (blocks_to_reserve == 0 || desired_extent_block >= total_blocks) {
        return (Extent){0, 0};
    }

    // try desired location first — enables merging with the last extent
    bool desired_free = true;
    if (desired_extent_block + blocks_to_reserve > total_blocks) {
        desired_free = false;
    } else {
        for (size_t i = desired_extent_block; i < desired_extent_block + blocks_to_reserve; i++) {
            if (get_bit(bitmap, i)) { desired_free = false; break; }
        }
    }
    if (desired_free) {
        for (size_t i = desired_extent_block; i < desired_extent_block + blocks_to_reserve; i++)
            set_bit(bitmap, i, 1);
        return (Extent){desired_extent_block, blocks_to_reserve};
    }

    // fall back to best-fit scan
    size_t best_start  = 0;
    size_t best_length = total_blocks + 1;
    bool   found_any   = false;
    size_t temp_count  = 0;
    size_t start_block_index = 0;

    for (size_t i = meta_blocks; i < total_blocks; i++) {
        if (!get_bit(bitmap, i)) {
            if (temp_count == 0) start_block_index = i;
            temp_count++;
        } else {
            if (temp_count >= blocks_to_reserve && temp_count < best_length) {
                best_start  = start_block_index;
                best_length = temp_count;
                found_any   = true;
                if (temp_count == blocks_to_reserve) break; // exact fit, stop early
            }
            temp_count = 0;
        }
    }
    // check the last run in case the disk ends while counting
    if (temp_count >= blocks_to_reserve && temp_count < best_length) {
        best_start  = start_block_index;
        found_any   = true;
    }

    if (found_any) {
        for (size_t j = 0; j < blocks_to_reserve; j++)
            set_bit(bitmap, best_start + j, 1);
        return (Extent){ best_start, blocks_to_reserve };
    }

    fprintf(stderr, "fs_allocate: Not enough contiguous space for %zu blocks.\n", blocks_to_reserve);
    return (Extent){0, 0};
}


ssize_t fs_create(FileSystem *fs) {
    // Validation check
    if (fs == NULL || fs->disk == NULL) {
        perror("fs_create: Error fs or disk is invalid (NULL)"); 
        return -1;
    }
    if (!fs->disk->mounted) { 
        fprintf(stderr, "fs_create: Error disk is not mounted, cannot procceed t\n");
        return -1;
    }

    int inode_num = -1;
    uint32_t *ibitmap = fs->ibitmap;
    uint32_t total_inodes = fs->meta_data->inodes;

    for (size_t i = 0; i < total_inodes; i++)
    {
        if (!(get_bit(ibitmap, i))) {
            inode_num = i;
            set_bit(ibitmap, i, 1);
            break;
        }
    }

    if (inode_num == -1) {
        set_bit(ibitmap, inode_num, 0);
        return -1;
    }

    uint32_t block_idx = 1 + (inode_num / INODES_PER_BLOCK);
    uint32_t offset = inode_num % INODES_PER_BLOCK;

    Block buffer;
    if (disk_read(fs->disk, block_idx, buffer.data) < 0) {
        set_bit(ibitmap, inode_num, 0);
        return -1;
    }


    Inode *target = &buffer.inodes[offset];
    target->valid = 1;
    target->size = 0;

    // Zeroing Out Extents
    target->extent_count = 0;
    target->extent_block = 0;
    for (size_t i = 0; i < EXTENTS_PER_INODE; i++) {
        target->extents[i].start = 0;
        target->extents[i].length = 0;
    }
    
    if (disk_write(fs->disk, block_idx, buffer.data) < 0) {
        set_bit(ibitmap, inode_num, 0);
        return -1;
    }
    return (ssize_t)inode_num;
}

ssize_t fs_write(FileSystem *fs, size_t inode_number, char *data, size_t length, size_t offset) 
{
    // Validation check
     if (fs == NULL || fs->disk == NULL) {
        perror("fs_write: Error fs or disk is invalid (NULL)"); 
        return -1;
    }
    if (!fs->disk->mounted) { 
        fprintf(stderr, "fs_write: Error disk is not mounted, cannot procceed t\n");
        return -1;
    }
    if (inode_number >= fs->meta_data->inodes) {
        // Inode number is invalid (too high)
        fprintf(stderr, "fs_write: Error inode_number is out of bounds, cannot procceed t\n");
        return -1;
    }

    // Figure out which logical blocks this write spans
    size_t start_logical_block = offset / BLOCK_SIZE;
    size_t start_block_offset = offset % BLOCK_SIZE;   // byte offset within the first block

    size_t end_byte = offset + length;                  // absolute end position in file
    size_t end_logical_block = (end_byte > 0) ? ((end_byte-1) / BLOCK_SIZE) : 0;

    // Locate the inode: block 0 is the superblock, so inode blocks start at 1
    uint32_t inode_block_idx = 1 + (inode_number / INODES_PER_BLOCK);
    uint32_t inode_offset_in_block = inode_number % INODES_PER_BLOCK;

    // Read the block containing our inode and get a pointer to it
    Block inode_buffer;
    if (disk_read(fs->disk, inode_block_idx, inode_buffer.data) < 0)
    {
        fprintf(stderr, "fs_write: Error writing has failed.\n");
        return -1;
    }
    Inode *target = &inode_buffer.inodes[inode_offset_in_block];

    // Walk through each logical block that this write touches
    size_t bytes_written = 0;
    for (size_t i = start_logical_block; i <= end_logical_block; i++)
    {
        // Determine the byte range within this block we need to write
        // First block may start mid-block, all others start at 0
        size_t block_start = 0;
        if (i == start_logical_block) {
            block_start = start_block_offset;
        }

        // Last block may end mid-block, all others go to BLOCK_SIZE
        size_t block_end = BLOCK_SIZE;
        if (i == end_logical_block) {
            block_end = end_byte % BLOCK_SIZE;
            if (block_end == 0) block_end = BLOCK_SIZE;
        }

        // Extents traverse
        uint32_t phys = extent_lookup(fs, target, i);
        // new block
        if (phys == 0) {
            Extent extent = fs_allocate(fs, 1, 0);
            if (extent.start == 0) {
                fprintf(stderr, "fs_write: Error extent allocation has failed.\n");
                return -1;
            }
            bool extent_added = extent_add(fs, target, extent.start, extent.length);
            if (!extent_added) {
                fprintf(stderr, "fs_write: Error adding extent has failed.\n");
                return -1;
            }
            phys = extent.start;
        }

        Block buffer;
        if (disk_read(fs->disk, phys, buffer.data) < 0) 
        {
            fprintf(stderr, "fs_write: Error reading from disk has failed.\n");
            return -1;
        }

        memcpy(buffer.data + block_start, data + bytes_written, block_end - block_start);
        
        if (disk_write(fs->disk, phys, buffer.data) < 0) {
            fprintf(stderr, "fs_write: Error writing to disk has failed.\n");
            return -1;
        }
        bytes_written += (block_end - block_start);
    }

    // Update file size if we extended past the previous end
    if (end_byte > target->size) {
        target->size = end_byte;
    }

    // Write the modified inode (updated pointers + size) back to disk
    if (disk_write(fs->disk, inode_block_idx, inode_buffer.data) < 0)
    {
        fprintf(stderr, "fs_write: Error writing has failed.\n");
        return -1;
    }

    // For now we save the bitmap after every single write until a solution comes up
    fs->bitmap->dirty = true;
    return bytes_written;
}

ssize_t fs_read(FileSystem *fs, size_t inode_number, char *data, size_t length, size_t offset) 
{
    // Validation check
     if (fs == NULL || fs->disk == NULL) {
        perror("fs_read: Error fs or disk is invalid (NULL)"); 
        return -1;
    }
    if (!fs->disk->mounted) { 
        fprintf(stderr, "fs_read: Error disk is not mounted, cannot procceed t\n");
        return -1;
    }
    if (inode_number >= fs->meta_data->inodes) {
        // Inode number is invalid (too high)
        fprintf(stderr, "fs_read: Error inode_number is out of bounds, cannot procceed t\n");
        return -1;
    }

    // Figure out which logical blocks this read spans
    size_t start_logical_block = offset / BLOCK_SIZE;
    size_t start_block_offset = offset % BLOCK_SIZE;   // byte offset within the first block

    // Locate the inode: block 0 is the superblock, so inode blocks start at 1
    uint32_t inode_block_idx = 1 + (inode_number / INODES_PER_BLOCK);
    uint32_t inode_offset_in_block = inode_number % INODES_PER_BLOCK;

    // Read the block containing our inode and get a pointer to it
    Block inode_buffer;
    if (disk_read(fs->disk, inode_block_idx, inode_buffer.data) < 0)
    {
        fprintf(stderr, "fs_read: Error reading has failed.\n");
        return -1;
    }

    Inode *target = &inode_buffer.inodes[inode_offset_in_block];

    if (!target->valid) 
    {
        fprintf(stderr, "fs_read: Inode is invalid.\n");
        return -1;
    }

    if (offset >= target->size) return 0;
    if (offset + length > target->size) length = target->size - offset;


    size_t end_byte = offset + length;                  // absolute end position in file
    size_t end_logical_block = (end_byte > 0) ? ((end_byte-1) / BLOCK_SIZE) : 0;

    size_t bytes_read = 0;

    for (size_t i = start_logical_block; i <= end_logical_block; i++) 
    {
        size_t block_start = (i == start_logical_block) ? start_block_offset : 0;
        size_t block_end = BLOCK_SIZE;
        if (i == end_logical_block) {
            block_end = end_byte % BLOCK_SIZE;
            if (block_end == 0) block_end = BLOCK_SIZE;
        }

        // Extents traverse
        uint32_t phys = extent_lookup(fs, target, i);
        // new block
        if (phys == 0) {
            memset(data + bytes_read, 0, block_end - block_start);
        }
        else {
            Block buffer;
            if (disk_read(fs->disk, phys, buffer.data) < 0) 
            {
                fprintf(stderr, "fs_read: Error reading from disk has failed.\n");
                return -1;
            }
            memcpy(data + bytes_read, buffer.data + block_start, block_end - block_start);
        }
        bytes_read += (block_end - block_start);
    }
    return bytes_read;
}


bool fs_remove(FileSystem *fs, size_t inode_number) 
{
    // Validation check
    if (fs == NULL || fs->disk == NULL) {
        perror("fs_remove: Error fs or disk is invalid (NULL)"); 
        return false;
    }
    if (!fs->disk->mounted) { 
        fprintf(stderr, "fs_remove: Error disk is not mounted, cannot procceed t\n");
        return false;
    }

    if (inode_number >= fs->meta_data->inodes) return false;

    // Locate the inode
    uint32_t inode_block_idx = 1 + (inode_number / INODES_PER_BLOCK);
    uint32_t inode_offset_in_block = inode_number % INODES_PER_BLOCK;

    // Read the block containing the inode
    Block inode_buffer;
    if (disk_read(fs->disk, inode_block_idx, inode_buffer.data) < 0) {
        fprintf(stderr, "fs_remove: Error reading inode block has failed.\n");
        return false;
    }
    Inode *target = &inode_buffer.inodes[inode_offset_in_block];

    if (!target->valid) {
        fprintf(stderr, "fs_remove: Inode is not valid.\n");
        return false;
    }

    // Cleaning the inode
    target->size = 0;
    target->valid = 0;
    for(size_t i = 0; i < EXTENTS_PER_INODE; i++) 
    {
        if (target->extents[i].start != 0 && target->extents[i].length) {
            for (size_t j = 0; j < target->extents[i].length; j++) {
                set_bit(fs->bitmap->bits, target->extents[i].start + j, 0);
            }
            target->extents[i].start = 0;
            target->extents[i].length = 0;
        }
    }
    if (target->extent_block != 0) 
    {
        Block extents_buf;
        if (disk_read(fs->disk, target->extent_block, extents_buf.data) < 0) {
            return false;
        }

        for (size_t i = 0; i < EXTENTS_PER_BLOCK; i++) 
        {
            Extent *extent_ptr = &extents_buf.extents[i];
            for (size_t j = 0; j < extent_ptr->length; j++)
                set_bit(fs->bitmap->bits, extent_ptr->start + j, 0);
        }

        memset(extents_buf.data, 0, BLOCK_SIZE);
        if (disk_write(fs->disk, target->extent_block, extents_buf.data) < 0) {
            fprintf(stderr, "fs_remove: Error failed to write to disk\n");
            return false;
        }

        set_bit(fs->bitmap->bits, target->extent_block, 0); 
        target->extent_block = 0;
        target->extent_count = 0;
    }
    // Write the modified inode back to disk
    if (disk_write(fs->disk, inode_block_idx, inode_buffer.data) < 0) {
        return false;
    }

    // Mark inode as free in ibitmap
    set_bit(fs->ibitmap, inode_number, 0);

    // Mark dirty — will be flushed on fs_unmount
    fs->bitmap->dirty = true;
    return true;
}

ssize_t fs_stat(FileSystem *fs, size_t inode_number) 
{
    // Validation check
    if (fs == NULL || fs->disk == NULL) {
        perror("fs_stat: Error fs or disk is invalid (NULL)"); 
        return -1;
    }
    if (!fs->disk->mounted) { 
        fprintf(stderr, "fs_stat: Error disk is not mounted, cannot procceed t\n");
        return -1;
    }

    if (inode_number >= fs->meta_data->inodes) return -1;

    // Locate the inode
    uint32_t inode_block_idx = 1 + (inode_number / INODES_PER_BLOCK);
    uint32_t inode_offset_in_block = inode_number % INODES_PER_BLOCK;

    // Read the block containing the inode
    Block inode_buffer;
    if (disk_read(fs->disk, inode_block_idx, inode_buffer.data) < 0) {
        fprintf(stderr, "fs_stat: Error reading inode block has failed.\n");
        return -1;
    }
    Inode *target = &inode_buffer.inodes[inode_offset_in_block];

    if (!target->valid) {
        return -1;
    } else {
        return target->size;
    }
}

ssize_t fs_lookup(FileSystem *fs, const char *path) 
{
    // Validation check
    if (fs == NULL || fs->disk == NULL || path == NULL) {
        perror("fs_lookup: Error fs, disk or path is invalid (NULL)"); 
        return -1;
    }
    if (!fs->disk->mounted) { 
        fprintf(stderr, "fs_lookup: Error disk is not mounted, cannot procceed t\n");
        return -1;
    }
    if (strcmp(path, "/") == 0) return 0; // root dir

    char path_copy[256];
    strncpy(path_copy, path, 255);
    path_copy[255] = '\0';

    size_t current_inode = 0; // start at root
    char *token = strtok(path_copy, "/");

    while (token != NULL) {
        ssize_t next = dir_lookup(fs, current_inode, token);
        if (next == -1) return -1; // component not found
        current_inode = (size_t)next;
        token = strtok (NULL, "/");
    }
    return (ssize_t)current_inode;
}

uint32_t extent_lookup(FileSystem *fs, Inode *inode, uint32_t logical_block) 
{
    if (fs == NULL || fs->disk == NULL) 
    {
        perror("extent_lookup: Error fs or disk is invalid (NULL)"); 
        return 0;
    }
    if (inode == NULL) {
        perror("extent_lookup: Error inode is invalid"); 
        return 0;
    }

    uint32_t base = 0;

    for (uint32_t i = 0; i < inode->extent_count && i < EXTENTS_PER_INODE; i++) 
    {
        if (logical_block >= base && logical_block < base + inode->extents[i].length) 
        {
            return inode->extents[i].start + (logical_block - base);
        }
        base += inode->extents[i].length;
    }

    // extents block
    if (inode->extent_block != 0) {
        Block buffer;
        if (disk_read(fs->disk, inode->extent_block, buffer.data) < 0) {
            perror("extent_lookup: Error reading from disk has failed"); 
            return 0;
        }
        Extent *extents_ptr = (Extent *)buffer.data; // Pointer to traverse around the extents block
        uint32_t overflow_count = inode->extent_count - EXTENTS_PER_INODE;

        for (uint32_t i = 0; i < overflow_count; i++) 
        {
            if (logical_block >= base && logical_block < base + extents_ptr[i].length) {
                return extents_ptr[i].start + (logical_block - base);
            }
            base += extents_ptr[i].length;
        }
    }
    return 0; // not found
}

Inode* fs_read_inode(FileSystem *fs, size_t inode_number) 
{
    if (fs == NULL || fs->disk == NULL) 
    {
        perror("fs_read_inode: Error fs or disk is invalid (NULL)"); 
        return NULL;
    }
    if (inode_number > fs->meta_data->inodes) {
        perror("fs_read_inode: Error inode_number exceeds the total inodes count"); 
        return NULL;
    }

    Block buffer;
    size_t block_idx = 1 + (inode_number / INODES_PER_BLOCK);
    size_t offset = inode_number % INODES_PER_BLOCK;
    if (disk_read(fs->disk, block_idx, buffer.data) < 0) 
    {
        perror("fs_read_inode: Error reading from disk has failed"); 
        return NULL;
    }
    Inode *inode = malloc(sizeof(Inode));
    if (inode == NULL) 
    {
        return NULL;
    }
    *inode = buffer.inodes[offset]; // copy the struct
    return inode;
}

bool extent_add(FileSystem *fs, Inode *inode, uint32_t start, uint32_t length)
{
    if (fs == NULL || fs->disk == NULL)
    {
        perror("extent_add: Error fs or disk is invalid (NULL)");
        return false;
    }
    if (start >= fs->meta_data->blocks) {
        perror("extent_add: Error start block exceeds disk capacity");
        return false;
    }
    if (inode == NULL)
    {
        perror("extent_add: Error given inode is invalid (NULL)");
        return false;
    }

    // starting from the first extent if the extents are empty for an inode
    if (inode->extent_count == 0)
    {
        inode->extents[0].start = start;
        inode->extents[0].length = length;
        inode->extent_count++;
        return true;
    }

    // Merge into existing extent
    if (inode->extent_count > 0 && inode->extent_count <= EXTENTS_PER_INODE)
    {
        Extent *last = &inode->extents[inode->extent_count-1]; // get the last extent
        if (last->start + last->length == start) {
            last->length += length;
            return true;
        }
    }

    if (inode->extent_count < EXTENTS_PER_INODE) {
        inode->extents[inode->extent_count].start = start;
        inode->extents[inode->extent_count].length = length;
        inode->extent_count++;
        return true;
    }

    // overflow case
    if (inode->extent_count >= EXTENTS_PER_INODE) {
        // allocate extents block if empty
        if (inode->extent_block == 0) 
        {
            Block buffer;
            memset(buffer.data, 0, BLOCK_SIZE);
            Extent extent = fs_allocate(fs, 1, 0);
            if (disk_write(fs->disk, extent.start, buffer.data) < 0) {
                perror("extent_add: Error writing to disk has failed");
                return false;
            }
            inode->extent_block = extent.start;
        }
        
        Block extents_buf;
        if (disk_read(fs->disk, inode->extent_block, extents_buf.data) < 0) {
            perror("extent_add: Error reading from disk has failed");
            return false;
        }

        
        size_t overflow_count = inode->extent_count - EXTENTS_PER_INODE;
        if (overflow_count > 0) {
            Extent *extent_ptr = &extents_buf.extents[overflow_count - 1];

            // contiguous case
            if ((extent_ptr->start + extent_ptr->length) == start) 
            {
                extent_ptr->length += length;
                if (disk_write(fs->disk, inode->extent_block, extents_buf.data) < 0 ) {
                    perror("extent_add: Error writing to disk has failed");
                    return false;
                }
                return true;
            }
            // non-contiguous case
            else if (overflow_count < EXTENTS_PER_BLOCK) {
                extent_ptr++;
                extent_ptr->start = start;
                extent_ptr->length = length;
                inode->extent_count++;
                if (disk_write(fs->disk, inode->extent_block, extents_buf.data) < 0) {
                    perror("extent_add: Error writing to disk has failed");
                    return false;
                }
                return true;
            }
            else {
                perror("extent_add: Error extents block is full");
                return false;
            }
        }
        if (overflow_count == 0) 
        {
            Extent *extent_ptr = &extents_buf.extents[0];
            extent_ptr->start = start;
            extent_ptr->length = length;
            inode->extent_count++;
            if (disk_write(fs->disk, inode->extent_block, extents_buf.data) < 0) {
                perror("extent_add: Error writing to disk has failed");
                return false;
            }
            return true;
        }

    }

    fprintf(stderr, "extent_add: No space left in extent block.\n");
    return false;
}