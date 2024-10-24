/*
  FUSE ssd: FUSE ioctl example
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>
  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/
#define FUSE_USE_VERSION 35
#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "ssd_fuse_header.h"
#define SSD_NAME "ssd_file"
enum
{
    SSD_NONE,
    SSD_ROOT,
    SSD_FILE,
};

static size_t erase_counts[PHYSICAL_NAND_NUM];
static size_t total_lbas;
static size_t physic_size;
static size_t logic_size;
static size_t host_write_size;
static size_t nand_write_size;
static int* page_valid;
static int GC_flag;

// The union of PCA rules is used to represent the physical address
typedef union pca_rule PCA_RULE;
union pca_rule
{
    unsigned int pca; // Physical Cluster Address
    struct
    {
        unsigned int page : 16; // Page number
        unsigned int block: 16; // Block number
    } fields;
};

PCA_RULE curr_pca; // Current PCA

unsigned int* L2P; // Logical to Physical 
unsigned int* P2L; // Physical to Logical

static int ftl_read(char* buf, size_t lba);
static int ftl_write(const char* buf, size_t lba_range, size_t lba);
static int ftl_gc();

// Adjust the logical size of the SSD
static int ssd_resize(size_t new_size)
{
    // Check if the new size exceeds the capacity of the logical NAND
    if (new_size > LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024)
    {
        // Out of memory error
        return -ENOMEM;
    }
    else
    {
        // Calculate the new total LBA number
        size_t new_total_lbas = new_size / 512;
        if (new_total_lbas > total_lbas)
        {
            // Reallocate L2P mapping table
            unsigned int* new_L2P = realloc(L2P, new_total_lbas * sizeof(*L2P));
            if (new_L2P == NULL)
            {
                printf("Failed to reallocate memory for L2P mapping.\n");
                return -ENOMEM;
            }
            L2P = new_L2P;

            // Initialize the new L2P mapping table part
            for (size_t i = total_lbas; i < new_total_lbas; i++)
            {
                L2P[i] = INVALID_PCA;
            }
        }

        // Set logic size to new_size
        logic_size = new_size;
        return 0;
    }

}

// Expand the logical size of the SSD
static int ssd_expand(size_t new_size)
{
    // Logical size must be greater than current size to expand
    if (new_size > logic_size)
    {
        return ssd_resize(new_size);
    }

    return 0;
}

// Read data from NAND
static int nand_read(char* buf, int pca)
{
    char nand_name[100];
    FILE* fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;

    // Generate the path of NAND file
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.block);

    // Open the corresponding NAND file for reading
    if ( (fptr = fopen(nand_name, "r") ))
    {
        // Locate the corresponding page
        fseek( fptr, my_pca.fields.page * 512, SEEK_SET );

        // Read 512 bytes of data
        fread(buf, 1, 512, fptr);
        fclose(fptr);
    }
    else
    {
        printf("open file fail at nand read pca = %d\n", pca);
        return -EINVAL;
    }
    // Return the number of bytes read
    return 512;
}

// Write data to NAND
static int nand_write(const char* buf, int pca)
{
    char nand_name[100];
    FILE* fptr;

    PCA_RULE my_pca;
    my_pca.pca = pca;

    // Generate the path of NAND file
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, my_pca.fields.block);

    // Open the corresponding NAND file for writing
    if ( (fptr = fopen(nand_name, "r+")))
    {
        // Locate the corresponding page
        fseek( fptr, my_pca.fields.page * 512, SEEK_SET );
        
        // Write 512 bytes of data
        fwrite(buf, 1, 512, fptr);
        fclose(fptr);
    }
    else
    {
        printf("open file fail at nand (%s) write pca = %d, return %d\n", nand_name, pca, -EINVAL);
        return -EINVAL;
    }

    // Update the total amount actually written to NAND
    nand_write_size += 512;

    // Return the number of bytes written
    return 512;
}

// Erase the specified NAND block
static int nand_erase(int block)
{
    char nand_name[100];
    FILE* fptr;

    // Generate the path of the NAND file
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, block);

    // Open the file in write mode to erase nand
    if ( (fptr = fopen(nand_name, "w")))
    {
        fclose(fptr);

        // Calculate the number of valid pages erased
        size_t pages_erased  = 0;
        for (size_t i = 0; i < PAGES_PER_BLOCK; i++)
        {
            size_t index = block * PAGES_PER_BLOCK + i;
            if (page_valid[index] != 0)
            {
                pages_erased ++;
                page_valid[index] = 0; // Mark as invalid
                P2L[index] = INVALID_LBA;
            }
        }

        // Decrease physic_size
        if (physic_size >= pages_erased)
            physic_size -= pages_erased;
        else
            physic_size = 0;

        erase_counts[block]++;

        printf("nand erase %d pass, erased %zu valid pages\n", block, pages_erased);

        return 1;
    }
    else
    {
        printf("open file fail at nand (%s) erase nand = %d, return %d\n", nand_name, block, -EINVAL);
        return -EINVAL;
    }
}

// Get the next available PCA (physical cluster address)
static unsigned int get_next_pca()
{
    /*  TODO: seq A, need to change to seq B */
    // Sequential allocation strategy B
    size_t total_pages = PHYSICAL_NAND_NUM * PAGES_PER_BLOCK ;

	// Initialize curr_pca if it's invalid
    if (curr_pca.pca == INVALID_PCA)
    {
        curr_pca.fields.block = 0;
        curr_pca.fields.page = 0;

        // Wrap around to the first block if necessary
        if (curr_pca.fields.block >= PHYSICAL_NAND_NUM)
        {
            curr_pca.fields.block = 0;
        }
    }
    else
    {
        // Move to next page
        curr_pca.fields.page += 1;

        // If the current block's pages are exhausted, move to the next block
        if (curr_pca.fields.page >= PAGES_PER_BLOCK)
        {
            curr_pca.fields.page = 0;
            curr_pca.fields.block += 1;

            // Wrap around to the first block if necessary
            if (curr_pca.fields.block >= PHYSICAL_NAND_NUM)
            {
                curr_pca.fields.block = 0;
            }
        }
    }

    // Sequential allocation strategy B

    // Keep track of the number of pages checked
    size_t pages_checked = 0;

    while (pages_checked < total_pages)
    {
        size_t index = curr_pca.fields.block * PAGES_PER_BLOCK + curr_pca.fields.page;

        // Check if the page is invalid (-1 means invalid)
        if (page_valid[index] == 0)
        {
            // Found a unused page
            curr_pca.pca = (curr_pca.fields.block << 16) | curr_pca.fields.page;
            printf("Allocated PCA: block %u, page %u\n", curr_pca.fields.block, curr_pca.fields.page);
            return curr_pca.pca;
        }

        // Move to next page
        curr_pca.fields.page += 1;

        // If the current block's pages are exhausted, move to the next block
        if (curr_pca.fields.page >= PAGES_PER_BLOCK)
        {
            curr_pca.fields.page = 0;
            curr_pca.fields.block += 1;
        }

        // Wrap around to the first block if necessary
        if (curr_pca.fields.block >= PHYSICAL_NAND_NUM)
        {
            curr_pca.fields.block = 0;
        }

        pages_checked++;
    }

    // No free pages available, SSD is full
    printf("No new PCA available, SSD is full\n");
    return FULL_PCA;
}

// FTL read operation
static int ftl_read( char* buf, size_t lba)
{
    /*  TODO: 1. Check L2P to get PCA 2. Send read data into nand_read */
    // Check if LBA is out of range
    if (lba >= total_lbas)
    {
        printf("Invalid PCA: Out of Index Range!\n");
        return -EINVAL;
    }
    else
    {
        PCA_RULE pca;
        // Get the PCA corresponding to LBA
        pca.pca = L2P[lba]; 

        // If PCA is invalid, it means that the data does not exist
        if (pca.pca == INVALID_PCA)
        {
            printf("Invalid PCA: Data does not exist!\n");
            return -EINVAL;
        }
        
        // Read data from NAND
        if (nand_read(buf, pca.pca) != 512)
        {
            printf("NAND read failed!\n");
            return -EIO;
        }

        // Return the number of bytes read
        return 512;
    }
}

// FTL write operation
static int ftl_write(const char* buf, size_t lba_range, size_t lba)
{
    /*  TODO: only basic write case, need to consider other cases */
    // Check if LBA is out of range
    if (lba >= total_lbas)
    {
        printf("Invalid LBA: Out of range!\n");
        return -EINVAL;
    }

    // Invalidate old PCA
    if (L2P[lba] != INVALID_PCA)
    {
        unsigned int old_pca = L2P[lba];
        printf("set block %d page %d invalid\n", (old_pca >> 16) & 0xFFFF, old_pca & 0xFFFF);
        size_t old_index = ((old_pca >> 16) & 0xFFFF) * PAGES_PER_BLOCK + (old_pca & 0xFFFF);
        if (old_index >= PHYSICAL_NAND_NUM * PAGES_PER_BLOCK)
        {
            printf("Error: old_index %zu out of range.\n", old_index);
            return -EINVAL;
        }
        if (page_valid[old_index] != -1)
        {
            page_valid[old_index] = -1;
            P2L[old_index] = INVALID_LBA;
        }
    }

    // Get the next available PCA
    PCA_RULE pca;
    pca.pca = get_next_pca();
    // If SSD is full, try garbage collection
    if (pca.pca == FULL_PCA)
    {
        printf("SSD is full, attempting garbage collection...\n");
        if (ftl_gc() != 0)
        {
            printf("Garbage collection failed, cannot write data!\n");
            return -ENOMEM;
        }

        // Reacquire PCA
        pca.pca = get_next_pca();
        if (pca.pca == FULL_PCA)
        {
            printf("No available PCA after garbage collection!\n");
            return -ENOMEM;
        }
    }

    // Write data to NAND
    if (nand_write(buf, pca.pca) > 0)
    {
        // Update L2P mapping
        if (lba < total_lbas) {
            L2P[lba] = pca.pca;
            printf("Updated L2P[%zu] = 0x%X\n", lba, pca.pca);
        } else {
            printf("Error: LBA %zu out of range when updating L2P.\n", lba);
            return -EINVAL;
        }

        // Update P2L mapping
        size_t new_index = pca.fields.block * PAGES_PER_BLOCK + pca.fields.page;
        if (new_index < PHYSICAL_NAND_NUM * PAGES_PER_BLOCK) {
            P2L[new_index] = lba;
            printf("Updated P2L[%zu] = %zu\n", new_index, lba);
        } else {
            printf("Error: P2L index %zu out of range when updating P2L.\n", new_index);
            return -EINVAL;
        }

        // Mark the new page as valid
        page_valid[new_index] = 1;

        // Increase physical size
        physic_size++;

        printf("block %d, page %d is mapping to %zu\n", pca.fields.block, pca.fields.page, lba);
        return 512;
    }
    else
    {
        printf(" --> Write fail !!!\n");
        return -EINVAL;
    }
}



// Counts the number of invalid pages in the specified block
static size_t count_invalid_pages(size_t block)
{
    size_t invalid_pages = 0;
    for (size_t page = 0; page < PAGES_PER_BLOCK ; page++)
    {
        size_t index = block * PAGES_PER_BLOCK + page;
        if(page_valid[index] == -1)
            invalid_pages++;
    }
    return invalid_pages;
}

// Select the block with the most invalid pages
static int select_block_for_gc()
{
    int block_to_erase = -1;
    size_t max_invalid_pages = 0;
    size_t min_erase_count = SIZE_MAX;

    for (size_t block = 0; block < PHYSICAL_NAND_NUM; block++)
    {
        size_t invalid_pages = count_invalid_pages(block);
        if (invalid_pages > max_invalid_pages)
        {
             // Prefer blocks with more invalid pages
            max_invalid_pages = invalid_pages;
            block_to_erase = block;
            min_erase_count = erase_counts[block];
        }
        else if (invalid_pages == max_invalid_pages && erase_counts[block] < min_erase_count)
        {
            // If multiple blocks have the same number of invalid pages, choose the one with lower erase count
            block_to_erase = block;
            min_erase_count = erase_counts[block];
        }
        printf("In Block %zu founded %zu invalid pages\n", block, invalid_pages);
    }

    if (block_to_erase != -1)
    {
        return block_to_erase;
    }
    else
    {
        // No block suitable for erasing
        return -1;
    }
}


// FTL garbage collection
static int ftl_gc()
{
    int block_to_erase = select_block_for_gc();
    GC_flag = 1;
    if (block_to_erase == -1)
    {
        printf("No suitable block found for garbage collection.\n");
        GC_flag = 0;
        return -EINVAL;
    }

    printf("Selected block %d for garbage collection.\n", block_to_erase);

    // Traverse each page in the block
    for (size_t page = 0; page < PAGES_PER_BLOCK; page++)
    {
        size_t index = block_to_erase * PAGES_PER_BLOCK + page;
        if (index >= PHYSICAL_NAND_NUM * PAGES_PER_BLOCK)
        {
            printf("Error: index %zu out of range during GC.\n", index);
            continue;
        }

        if (page_valid[index] == 1)
        {
            char page_buf[512];
            // Use P2L mapping table to find the corresponding LBA
            size_t lba = P2L[index];

            if (lba == INVALID_LBA)
            {
                printf("No corresponding LBA found for PCA (%d, %zu).\n", block_to_erase, page);
                continue;
            }

            // Read valid data from NAND
            if (ftl_read(page_buf, lba) < 0)
            {
                printf("Failed to read data from LBA %zu during GC.\n", lba);
                GC_flag = 0;
                return -EIO;
            }

            // Write data to new PCA
            if (ftl_write(page_buf, 1, lba) < 0)
            {
                printf("Failed to write data to new PCA during GC.\n");
                GC_flag = 0;
                return -EIO;
            }

            // Mark old PCA as invalid
            P2L[index] = INVALID_LBA;
            page_valid[index] = -1;
        }
    }

    // Erase block
    if (nand_erase(block_to_erase) != 1)
    {
        printf("Failed to erase block %d during GC.\n", block_to_erase);
        GC_flag = 0;
        return -EIO;
    }

    printf("Garbage collection for block %d completed successfully.\n", block_to_erase);
    GC_flag = 0;
    return 0;
}

// Determine the file type
static int ssd_file_type(const char* path)
{
    if (strcmp(path, "/") == 0)
    {
        return SSD_ROOT;
    }
    if (strcmp(path, "/" SSD_NAME) == 0)
    {
        return SSD_FILE;
    }
    return SSD_NONE;
}

// Get file attributes
static int ssd_getattr(const char* path, struct stat* stbuf,
                       struct fuse_file_info* fi)
{
    (void) fi;

    // User ID of file owner
    stbuf->st_uid = getuid();

    // Group ID of file owner
    stbuf->st_gid = getgid();

    // Access and modification time
    stbuf->st_atime = stbuf->st_mtime = time(NULL);
    switch (ssd_file_type(path))
    {
        case SSD_ROOT:
            // Directory permissions
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
            break;
        case SSD_FILE:
            // File permissions
            stbuf->st_mode = S_IFREG | 0644;
            stbuf->st_nlink = 1;

            // File size
            stbuf->st_size = logic_size;
            break;
        case SSD_NONE:
            // File does not exist
            return -ENOENT;
    }
    return 0;
}

// Open file
static int ssd_open(const char* path, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_NONE)
    {
        return 0;
    }
    return -ENOENT;
}

// Actual implementation of reading data
static int ssd_do_read(char* buf, size_t size, off_t offset)
{
    /*  TODO: call ftl_read function and handle result */
    int tmp_lba, tmp_lba_range, idx, ret;
    size_t process_size = 0;
    size_t remain_size = size;

    // Check if the read range out of limit
    if (offset >= logic_size)
    {
        return 0;
    }
    if (size > logic_size - offset)
    {
        // Adjust read size
        size = logic_size - offset;
    }

    // Calculate the starting LBA
    tmp_lba = offset / 512;

    // Calculate the number of LBAs to be read
	tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;


    for (idx = 0; idx < tmp_lba_range; idx++)
    {
        char page_buf[512];
        size_t page_offset = (offset + process_size) % 512;
        size_t read_size = (remain_size < (512 - page_offset)) ? remain_size : (512 - page_offset);

        // Check if LBA exists in L2P mapping
        if (L2P[tmp_lba + idx] != INVALID_PCA)
        {
            ret = ftl_read(page_buf, tmp_lba + idx);
            if (ret < 0)
            {
                return ret;
            }
        }
        else
        {
            memset(page_buf, 0x00, 512); // Assuming unread pages return 0x00
        }

        memcpy(buf + process_size, page_buf + page_offset, read_size);

        process_size += read_size;
        remain_size -= read_size;
    }

    return size;
}

// Read file
static int ssd_read(const char* path, char* buf, size_t size,
                    off_t offset, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_read(buf, size, offset);
}

// Actual write file
static int ssd_do_write(const char* buf, size_t size, off_t offset)
{
    /*  TODO: only basic write case, need to consider other cases */
    int tmp_lba, tmp_lba_range;
    size_t process_size = 0;
    size_t remain_size = size;
    int idx, ret;

    
    // Check and expand the logical size
    if (ssd_expand(offset + size) != 0)
    {
        return -ENOMEM;
    }

    // Update the total amount of data written by the host
    host_write_size += size;

    // Starting LBA
    tmp_lba = offset / 512;

    // Number of LBAs to be written
    tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;

    for (idx = 0; idx < tmp_lba_range; idx++)
    {
        char page_buf[512];
        size_t page_offset = (offset + process_size) % 512;
        size_t write_size = (remain_size < (512 - page_offset)) ? remain_size : (512 - page_offset);

        // Read the existing data if it's a partial page write
        if (page_offset != 0 || write_size < 512)
        {
            // Check if LBA exists in L2P mapping
            if (L2P[tmp_lba + idx] != INVALID_PCA)
            {
                ret = nand_read(page_buf, L2P[tmp_lba + idx]);
                if (ret < 0)
                {
                    return ret;
                }
            }
            else
            {
                memset(page_buf, 0x00, 512); // Initialize with empty data (assuming NAND is erased to 0x00)
            }
            // Update the necessary portion
            memcpy(page_buf + page_offset, buf + process_size, write_size);
        }
        else
        {
            // Full page write
            memcpy(page_buf, buf + process_size, 512);
        }

        // Write the page data
        ret = ftl_write(page_buf, 1, tmp_lba + idx);
        if (ret < 0)
        {
            return ret;
        }

        process_size += write_size;
        remain_size -= write_size;
    }

    return size;
}

// Write file
static int ssd_write(const char* path, const char* buf, size_t size,
                     off_t offset, struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    return ssd_do_write(buf, size, offset);
}

// Truncate file
static int ssd_truncate(const char* path, off_t size,
                        struct fuse_file_info* fi)
{
    (void) fi;
    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }

    return ssd_resize(size);
}

// Read directory
static int ssd_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info* fi,
                       enum fuse_readdir_flags flags)
{
    (void) fi;
    (void) offset;
    (void) flags;
    if (ssd_file_type(path) != SSD_ROOT)
    {
        return -ENOENT;
    }
    // Current directory
    filler(buf, ".", NULL, 0, 0);
    // Upper-level directory
    filler(buf, "..", NULL, 0, 0);
    // SSD file
    filler(buf, SSD_NAME, NULL, 0, 0);
    return 0;
}

// Handle ioctl command
static int ssd_ioctl(const char* path, unsigned int cmd, void* arg,
                     struct fuse_file_info* fi, unsigned int flags, void* data)
{

    if (ssd_file_type(path) != SSD_FILE)
    {
        return -EINVAL;
    }
    if (flags & FUSE_IOCTL_COMPAT)
    {
        return -ENOSYS;
    }
    switch (cmd)
    {
        case SSD_GET_LOGIC_SIZE:
            *(size_t*)data = logic_size;
            printf(" --> logic size: %zu\n", logic_size);
            return 0;
        case SSD_GET_PHYSIC_SIZE:
            *(size_t*)data = physic_size;
            printf(" --> physic size: %zu\n", physic_size);
            return 0;
        case SSD_GET_WA:
            *(double*)data = (double)nand_write_size / (double)host_write_size;
            return 0;
    }
    return -EINVAL;
}

// Define FUSE operation
static const struct fuse_operations ssd_oper =
{
    .getattr        = ssd_getattr,
    .readdir        = ssd_readdir,
    .truncate       = ssd_truncate,
    .open           = ssd_open,
    .read           = ssd_read,
    .write          = ssd_write,
    .ioctl          = ssd_ioctl,
};

int main(int argc, char* argv[])
{
    int idx;
    char nand_name[100];
    physic_size = 0;
    logic_size = 0;
	nand_write_size = 0;
	host_write_size = 0;
    curr_pca.pca = INVALID_PCA;

    // Not in GC
    GC_flag = 0;

    // Calculate the total number of LBAs
    total_lbas = LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024 / 512;

    // Allocate memory space for L2P mapping table
    L2P = malloc(total_lbas * sizeof(*L2P));
    if (L2P == NULL)
    {
        printf("Failed to allocate memory for L2P mapping.\n");
        return -1;
    }

    // Initialize L2P mapping table
    for (size_t i = 0; i < total_lbas; i++)
    {
        L2P[i] = INVALID_PCA;
    }
    
    // Allocate physical page validity bitmap
    page_valid = (int *)malloc(sizeof(int) * PHYSICAL_NAND_NUM * PAGES_PER_BLOCK);
    if (page_valid == NULL)
    {
        printf("Failed to allocate memory for page valid bitmap.\n");
        free(L2P);
        return -1;
    }

    // Initialize to 0, indicating that all physical pages are unused
    for(size_t i=0; i < PHYSICAL_NAND_NUM * PAGES_PER_BLOCK; i++)
    {
        page_valid[i] = 0;
    }

    // Allocate memory space for P2L mapping table
    P2L = malloc(PHYSICAL_NAND_NUM * PAGES_PER_BLOCK * sizeof(*P2L));
    if (P2L == NULL)
    {
        printf("Failed to allocate memory for P2L mapping.\n");
        free(L2P);
        free(page_valid);
        return -1;
    }

    // Initialize P2L mapping table
    for (size_t i = 0; i < PHYSICAL_NAND_NUM * PAGES_PER_BLOCK; i++)
    { 
        P2L[i] = INVALID_LBA;
    }

    // Create NAND files
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
    {
        FILE* fptr;
        snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, idx);
        fptr = fopen(nand_name, "w");
        if (fptr == NULL)
        {
            printf("Failed to create NAND file %s\n", nand_name);
            free(L2P);
            free(P2L);
            free(page_valid);
            return -1;
        }
        
        fclose(fptr);
    }

    // Initialize erase counts
    for (size_t block = 0; block < PHYSICAL_NAND_NUM; block++)
    {
        erase_counts[block] = 0;
    }

    // Start FUSE file system
    return fuse_main(argc, argv, &ssd_oper, NULL);
}