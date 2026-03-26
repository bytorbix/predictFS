#pragma once

#include "fs.h"
#include <stdbool.h>

#define ENTRY_SIZE (sizeof(ExtensionEntry))
#define ENTRIES_PER_BLOCK (BLOCK_SIZE / ENTRY_SIZE)

#define BUCKET_0_MAX (4096UL)
#define BUCKET_1_MAX (524288UL)
#define BUCKET_2_MAX (67108864UL)

#define HIGH_CONFIDENCE (0.70f)
#define LOW_CONFIDENCE  (0.40f)


typedef struct BucketStats BucketStats;
struct BucketStats {
    uint32_t lower_bound;
    uint32_t upper_bound;
    uint32_t count;
    float tendency;
    float mean_final_size;
    float m2_final_size;
    uint32_t growing_count; // denominator for ratio stats
    float mean_ratio;
    float m2_ratio;
};

typedef struct ExtensionEntry ExtensionEntry;
struct ExtensionEntry {
    char name[16];              // Extension name (.png, .log)
    BucketStats buckets[4];     // size buckets
};

typedef struct LiveFileEntry LiveFileEntry;
struct LiveFileEntry {
    uint32_t inode_number; 
    uint32_t first_write_size; // size after first write (0 = not written)
    char extension[16]; // file extension name
    uint32_t bucket_index;
};


typedef struct pFileSystem pFileSystem;
struct pFileSystem {
    FileSystem *fs; // filesystem instance
    ExtensionEntry *entries; // extension entries stats array (from disk)
    LiveFileEntry *live_files; // currently open files being tracked
    size_t live_count; // number of active file entries 
    size_t live_capacity; // capacity of file entries in the array
    bool dirty; // if data needs to be written to disk
};


bool pfs_mount(pFileSystem *pfs, Disk *disk);
bool pfs_format(Disk *disk);
bool pfs_unmount(pFileSystem *pfs);
ssize_t pfs_create(pFileSystem *pfs, const char *path);
ssize_t pfs_write(pFileSystem *pfs, size_t inode_number, char *data, size_t length, size_t offset);
ssize_t pfs_remove(pFileSystem *pfs, size_t inode_number);
ExtensionEntry* add_entry(pFileSystem *pfs, const ExtensionEntry *entry);
ExtensionEntry* find_entry(pFileSystem *pfs, const char *extension);
LiveFileEntry* add_live_entry(pFileSystem *pfs, LiveFileEntry *entry);
LiveFileEntry* find_live_entry(pFileSystem *pfs, size_t inode_number);
void remove_live_entry(pFileSystem *pfs, size_t inode_number);
uint32_t get_bucket_index(uint32_t first_write_size);
float pfs_confidence(BucketStats *bucket);
