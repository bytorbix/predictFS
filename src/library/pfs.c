#include "fs.h"
#include <stdbool.h>
#include "pfs.h"
#include "utils.h"
#include "dir.h"
#include <math.h>

bool pfs_format(Disk *disk)
{
    return fs_format(disk);
}

bool pfs_mount(pFileSystem *pfs, Disk *disk) 
{
    pfs->fs = calloc(1, sizeof(FileSystem));
    if (pfs->fs == NULL) return false;

    bool flag = fs_mount(pfs->fs, disk);
    if (flag) 
    {
        Block buffer;
        memset(buffer.data, 0, BLOCK_SIZE);
        size_t pfs_block = pfs->fs->meta_data->inode_blocks + pfs->fs->meta_data->bitmap_blocks + 1;
        if (disk_read(disk, pfs_block, buffer.data) < 0) {
            perror("pfs_mount: Failed to read from disk");
            return false;
        }

        pfs->entries = (ExtensionEntry*)calloc(ENTRIES_PER_BLOCK, sizeof(ExtensionEntry));
        if (pfs->entries == NULL) {
            return false;
        }
        ExtensionEntry *ptr = (ExtensionEntry *)buffer.data;

        for (size_t i = 0; i < ENTRIES_PER_BLOCK; i++) {
            if (ptr[i].name[0] != '\0') {
                memcpy(&pfs->entries[i], &ptr[i],sizeof(ExtensionEntry));
            }
        }
        return true;
    }
    else 
    {
        free(pfs->fs);
        free(pfs->entries);
        
        return false;
    }
}

ssize_t pfs_create(pFileSystem *pfs, const char *path) 
{
    if (pfs->fs == NULL) return false;
    // get inode
    ssize_t inode_file = fs_create(pfs->fs);
    // check if we managed to get allocated an inode
    if (inode_file != -1) {
        // File has been created and inode is given
        // extract the file components
        char *parentdir_path = extract_parentdir(path);
        char *filename = extract_filename(path);
        // validation checks
        if (parentdir_path == NULL || filename == NULL) 
        {
            free(parentdir_path);
            return -1;
        }
        // retrieve the parent directory inode
        ssize_t inode_parentdir = fs_lookup(pfs->fs, parentdir_path);
        if (inode_parentdir == -1) 
        {
            free(parentdir_path);
            return -1;
        }
        // adding the file entry into the directory
        if (dir_add(pfs->fs, inode_parentdir, filename, inode_file) < 0)
        {
            free(parentdir_path);
            return -1;
        }

        // Extract Extension and add it to the entries if the extension doesn't work
        char *extension = extract_extension(filename);
        if (extension != NULL) 
        {   
            ExtensionEntry tempEntry;
            strcpy(tempEntry.name, extension);
            ExtensionEntry *entry = add_entry(pfs, &tempEntry);
            if (entry == NULL) 
            {
                free(parentdir_path);
                return -1;
            }
            LiveFileEntry live_entry;
            live_entry.inode_number = inode_file;
            live_entry.first_write_size = 0;
            strncpy(live_entry.extension, extension, 16);
            if (add_live_entry(pfs, &live_entry) == NULL) {
                free(parentdir_path);
                return -1;
            }
            pfs->dirty = true;

        }
        // cleanup and return
        free(parentdir_path);
        return inode_file;
    }
    else {
        return -1;
    }
}

ssize_t pfs_write(pFileSystem *pfs, size_t inode_number, const char *data, size_t length, size_t offset) 
{
    if (pfs == NULL) {
        perror("pfs_write: Error pfs is invalid");
        return -1;
    }

    // pre allocate on first write
    LiveFileEntry *live = find_live_entry(pfs, inode_number);
    if (live != NULL && live->first_write_size == 0) {
        uint32_t first_size = (uint32_t)length;
        uint32_t bucket_idx = get_bucket_index(first_size);
        ExtensionEntry *ext = find_entry(pfs, live->extension);
        if (ext != NULL) {
            BucketStats *bucket = &ext->buckets[bucket_idx];
            float confidence = pfs_confidence(bucket);
            uint32_t predicted_size = 0;
            if (confidence >= HIGH_CONFIDENCE) {
                predicted_size = (uint32_t)(first_size * bucket->mean_ratio);
            } else if (confidence >= LOW_CONFIDENCE) {
                predicted_size = (uint32_t)(first_size * (1.0f + bucket->mean_ratio * 0.5f));
            }
            if (predicted_size > first_size) {
                uint32_t blocks = (predicted_size + BLOCK_SIZE - 1) / BLOCK_SIZE;
                Inode *inode = fs_read_inode(pfs->fs, inode_number);
                if (inode != NULL) {
                    Extent ext_alloc = fs_allocate(pfs->fs, blocks, 0);
                    if (ext_alloc.start != 0) {
                        extent_add(pfs->fs, inode, ext_alloc.start, ext_alloc.length);
                    }
                    free(inode);
                }
            }
        }
        live->first_write_size = first_size;
        live->bucket_index = bucket_idx;
        return fs_write(pfs->fs, inode_number, data, length, offset);
    }
    return fs_write(pfs->fs, inode_number, data, length, offset);
}

bool pfs_unmount(pFileSystem *pfs) 
{
    if (pfs == NULL) {
        perror("pfs_unmount: Error pfs is invalid");
        return false;
    }
    if (pfs->dirty) 
    {
        Block entries_block;
        size_t pfs_block = pfs->fs->meta_data->inode_blocks + pfs->fs->meta_data->bitmap_blocks + 1;
        memcpy(entries_block.data, pfs->entries, ENTRIES_PER_BLOCK*ENTRY_SIZE);
        if (disk_write(pfs->fs->disk, pfs_block, entries_block.data) < 0) 
        {
            perror("pfs_unmount: Failed writing to disk");
            return false;
        }
    }
    free(pfs->live_files);
    fs_unmount(pfs->fs);
    free(pfs->entries);
    free(pfs->fs);
    return true;
}

ssize_t pfs_remove(pFileSystem *pfs, size_t inode_number) {
    if (pfs == NULL) {
        perror("pfs_remove: Error pfs is invalid");
        return -1;
    }

    // find file entry
    LiveFileEntry *live = find_live_entry(pfs, inode_number);
    if (live == NULL || live->first_write_size == 0) {
        // skipping stats update and just fs_remove
        if (!fs_remove(pfs->fs, inode_number)) {
            perror("pfs_remove: Error fs_remove has failed");
            return -1;
        }
        return 0;
    }
    ssize_t file_final_size = fs_stat(pfs->fs, inode_number);
    bool did_grow = (file_final_size > live->first_write_size);
    ExtensionEntry *ExtEntry = find_entry(pfs, live->extension);

    if (ExtEntry != NULL) {
        BucketStats *bucket = &ExtEntry->buckets[live->bucket_index];
        bucket->count++;
        bucket->tendency += (did_grow - bucket->tendency) / bucket->count; // a calculation that increases mean for each file that grew and less for files whom didn't
        
        float delta = file_final_size - bucket->mean_final_size; // calculating distance from mean
        bucket->mean_final_size += delta / bucket->count;
        float delta2 = file_final_size - bucket->mean_final_size; // calculating the variance
        bucket->m2_final_size += delta * delta2;

        if (did_grow) {
            float ratio = (float)file_final_size / (float)live->first_write_size;
            bucket->growing_count++;
            float delta = ratio - bucket->mean_ratio;
            bucket->mean_ratio += delta / bucket->growing_count;
            float delta2 = ratio - bucket->mean_ratio;
            bucket->m2_ratio += delta * delta2;
        }

        pfs->dirty = true;
    }

    remove_live_entry(pfs, inode_number);

    if (!fs_remove(pfs->fs, inode_number)) {
        perror("pfs_remove: Error fs_remove has failed");
        return -1;
    }
    return 0;
}

ExtensionEntry* add_entry(pFileSystem *pfs, const ExtensionEntry *entry) 
{   
    // validation check
    if (entry->name[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < ENTRIES_PER_BLOCK; i++) 
    {
        ExtensionEntry *temp_entry = &pfs->entries[i];
        if (strcmp(temp_entry->name, entry->name) == 0) 
        {
            // duplicate entry, return existing
            return &pfs->entries[i];
        }
        if (*temp_entry->name == '\0') 
        {
            memset(&pfs->entries[i], 0, sizeof(ExtensionEntry));
            strncpy(pfs->entries[i].name, entry->name, 16);
            pfs->entries[i].buckets[0].lower_bound = 0;
            pfs->entries[i].buckets[0].upper_bound = BUCKET_0_MAX;
            pfs->entries[i].buckets[1].lower_bound = BUCKET_0_MAX;
            pfs->entries[i].buckets[1].upper_bound = BUCKET_1_MAX;
            pfs->entries[i].buckets[2].lower_bound = BUCKET_1_MAX;
            pfs->entries[i].buckets[2].upper_bound = BUCKET_2_MAX;
            pfs->entries[i].buckets[3].lower_bound = BUCKET_2_MAX;
            pfs->entries[i].buckets[3].upper_bound = UINT32_MAX;
            return &pfs->entries[i];
        }
    }
    return NULL;
}

ExtensionEntry* find_entry(pFileSystem *pfs, const char *extension) 
{
    // validation check
    if (extension == NULL || extension[0] == '\0' || pfs == NULL) 
    {
        perror("find_entry: extension or pfs given is invalid");
        return NULL;
    }
    for (size_t i = 0; i <ENTRIES_PER_BLOCK; i++) 
    {
        if (strcmp(pfs->entries[i].name, extension) == 0) 
        {
            // entry is found, returning pointer
            return &pfs->entries[i];
        }
    }
    // an entry with the extension is not found
    return NULL;
}

LiveFileEntry* add_live_entry(pFileSystem *pfs, LiveFileEntry *entry) {
    // validation check
    if (pfs == NULL) 
    {
        perror("add_live_entry: pfs given is invalid");
        return NULL;
    }
    if (pfs->live_count == pfs->live_capacity) {
        size_t new_capacity = (pfs->live_capacity == 0) ? 8 : pfs->live_capacity * 2;
        LiveFileEntry *new_live = realloc(pfs->live_files, new_capacity * sizeof(LiveFileEntry));
        if (new_live == NULL) return NULL;
        pfs->live_files = new_live;
        pfs->live_capacity = new_capacity;
    }
    pfs->live_files[pfs->live_count] = *entry;
    return &pfs->live_files[pfs->live_count++];
}

LiveFileEntry* find_live_entry(pFileSystem *pfs, size_t inode_number) {
    // validation check
    if (pfs == NULL || pfs->live_files == NULL) 
    {
        perror("find_live_entry: live_files or pfs given is invalid");
        return NULL;
    }

    for (size_t i = 0; i < pfs->live_count; i++) 
    {
        if (inode_number == pfs->live_files[i].inode_number) {
            return &pfs->live_files[i];
        }
    }
    // an entry file with the inode number is not found
    return NULL;
}

void remove_live_entry(pFileSystem *pfs, size_t inode_number) {
    // validation check
    if (pfs == NULL || pfs->live_files == NULL) 
    {
        perror("remove_live_entry: live_files or pfs given is invalid");
        return;
    }
    for (size_t i = 0; i < pfs->live_count; i++) {
        if (inode_number == pfs->live_files[i].inode_number) 
        {
            pfs->live_files[i] = pfs->live_files[pfs->live_count - 1];
            pfs->live_count--;
            return;
        }
    }
}


uint32_t get_bucket_index(uint32_t first_write_size) {
    if (first_write_size < BUCKET_0_MAX) return 0;
    if (first_write_size < BUCKET_1_MAX) return 1;
    if (first_write_size < BUCKET_2_MAX) return 2;
    return 3;
}

float pfs_confidence(BucketStats *bucket) {
    if (bucket == NULL || bucket->count == 0) return 0.0f;

    // sample weight (do we have enough data?)
    float sample_weight = (float)bucket->count / 50.0f;
    if (sample_weight > 1.0f) sample_weight = 1.0f;

    // tendency consistency (is the consistency data stable?)
    float t = bucket->tendency;
    float tendency_consistency = 1.0f - (4.0f * t * (1.0f -t));

    // ratio consistency (is growth amount predictable?)
    float ratio_consistency = 0.0f;
    if (bucket->growing_count > 1) {
        float ratio_std = sqrtf(bucket->m2_ratio / bucket->growing_count);
        float ratio_cv = ratio_std / bucket->mean_ratio;
        ratio_consistency = 1.0f - ratio_cv;
        if (ratio_consistency < 0.0f) ratio_consistency = 0.0f;
    }
    
    return sample_weight * tendency_consistency * ratio_consistency; 
}
