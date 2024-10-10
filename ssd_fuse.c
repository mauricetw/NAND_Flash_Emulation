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
#define SSD_NAME       "ssd_file"
enum
{
    SSD_NONE,
    SSD_ROOT,
    SSD_FILE,
};


static size_t physic_size;
static size_t logic_size;
static size_t host_write_size;
static size_t nand_write_size;

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

// Adjust the logical size of the SSD
static int ssd_resize(size_t new_size)
{
    // Check if the new size exceeds the capacity of the logical NAND
    if (new_size >= LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024  )
    {
        // Out of memory error
        return -ENOMEM;
    }
    else
    {
        // Set logic size to new_size
        logic_size = new_size;
        return 0;
    }

}

// Expand the logical size of the SSD
static int ssd_expand(size_t new_size)
{
    // Logical size must be less than logic limit
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
        
        // Update the physical write size
        physic_size ++;
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
	int found = 0;
    FILE* fptr;

    // Generate the path of the NAND file
    snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, block);

    // Open the file in write mode to erase nand
    if ( (fptr = fopen(nand_name, "w")))
    {
        fclose(fptr);
    }
    else
    {
        printf("open file fail at nand (%s) erase nand = %d, return %d\n", nand_name, block, -EINVAL);
        return -EINVAL;
    }


	if (found == 0)
	{
		printf("nand erase not found\n");
		return -EINVAL;
	}

    printf("nand erase %d pass\n", block);
    return 1;
}

// Get the next available PCA (physical cluster address)
static unsigned int get_next_pca()
{
    /*  TODO: seq A, need to change to seq B */

	// Initialize PCA
    if (curr_pca.pca == INVALID_PCA)
    {
        curr_pca.pca = 0;
        return curr_pca.pca;
    }
    // If the SSD is full, a new PCA cannot be allocated
    else if (curr_pca.fields.block == PHYSICAL_NAND_NUM - 1 && curr_pca.fields.page == NAND_SIZE_KB * 1024 / 512)
    {
        printf("No new PCA\n");
        return FULL_PCA;
    }


    /* Seq A
    if ( curr_pca.fields.block == PHYSICAL_NAND_NUM - 1)
    {
        curr_pca.fields.page += 1;
    }
    curr_pca.fields.block = (curr_pca.fields.block + 1 ) % PHYSICAL_NAND_NUM;

    if ( curr_pca.fields.page >= (NAND_SIZE_KB * 1024 / 512) )
    {
        printf("No new PCA\n");
        curr_pca.pca = FULL_PCA;
        return FULL_PCA;
    }
    else
    {
        printf("PCA = page %d, nand %d\n", curr_pca.fields.page, curr_pca.fields.block);
        return curr_pca.pca;
    }
    */
    // Seq B
    // Sequential allocation strategy B

    // If the page of the current block is full, switch to the next block
    if ( curr_pca.fields.page == (NAND_SIZE_KB * 1024 / 512) )
    {
        curr_pca.fields.block = (curr_pca.fields.block + 1 ) % PHYSICAL_NAND_NUM;
        curr_pca.fields.page = 0;
    }
    // Else write to next page 
    else curr_pca.fields.page += 1;
    printf("PCA = page %d, nand %d\n", curr_pca.fields.page, curr_pca.fields.block);
    return curr_pca.pca;

}

// FTL read operation
static int ftl_read( char* buf, size_t lba)
{
    /*  TODO: 1. Check L2P to get PCA 2. Send read data into nand_read */
    // Check if LBA is out of range
    if ( lba >= sizeof(L2P) || lba < 0)
    {
        printf("Invalid PCA: Out of Index Range!");
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
static int ftl_write(const char* buf, size_t lba_rnage, size_t lba)
{
    /*  TODO: only basic write case, need to consider other cases */
    // Get the next available PCA
    PCA_RULE pca;
    pca.pca = get_next_pca();

    // Write data to NAND
    if (nand_write( buf, pca.pca) > 0)
    {
        // Update L2P mapping
        L2P[lba] = pca.pca;
        return 512 ;
    }
    else
    {
        printf(" --> Write fail !!!\n");
        return -EINVAL;
    }
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
    int tmp_lba, tmp_lba_range, rst ;
    char* tmp_buf;

    // Check if the read range out of limit
    if ((offset) >= logic_size)
    {
        return 0;
    }
    if ( size > logic_size - offset)
    {
        // Adjust read size
        size = logic_size - offset;
    }

    // Calculate the starting LBA
    tmp_lba = offset / 512;

    // Calculate the number of LBAs to be read
	tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;
    tmp_buf = calloc(tmp_lba_range * 512, sizeof(char));

    for (int i = 0; i < tmp_lba_range; i++) {
        // TODO
        ftl_read(tmp_buf + i * 512,tmp_lba + i);
    }

    //Copy data to user buffer
    memcpy(buf, tmp_buf + offset % 512, size);

    free(tmp_buf);
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
	
    int tmp_lba, tmp_lba_range, process_size;
    int idx, curr_size, remain_size, rst;

    // Update the total amount of data written by the host
    host_write_size += size;

    // Check and expand the logical size
    if (ssd_expand(offset + size) != 0)
    {
        return -ENOMEM;
    }

    // Starting LBA
    tmp_lba = offset / 512;

    // Number of LBAs to be written
    tmp_lba_range = (offset + size - 1) / 512 - (tmp_lba) + 1;

    process_size = 0;
    remain_size = size;
    curr_size = 0;
    for (idx = 0; idx < tmp_lba_range; idx++)
    {
        /*  Example only align 512, need to implement other cases  */
        if(offset % 512 == 0 && size % 512 == 0)
        {
            rst = ftl_write(buf + process_size, 1, tmp_lba + idx);
            if ( rst == 0 )
            {
                // Full return error
                return -ENOMEM;
            }
            else if (rst < 0)
            {
                // Error
                return rst;
            }
            curr_size += 512;
            remain_size -= 512;
            process_size += 512;
            offset += 512;
        }
        else{
            printf(" --> Not align 512 !!!");
            return -EINVAL;
        }
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
            printf(" --> logic size: %ld\n", logic_size);
            return 0;
        case SSD_GET_PHYSIC_SIZE:
            *(size_t*)data = physic_size;
            printf(" --> physic size: %ld\n", physic_size);
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

    // Allocate memory space for L2P mapping table
    L2P = malloc(LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024 / 512 * sizeof(int));
    
    // Initialize L2P mapping table
    memset(L2P, INVALID_PCA, sizeof(int)*LOGICAL_NAND_NUM * NAND_SIZE_KB * 1024 / 512);

    // Create nand file
    for (idx = 0; idx < PHYSICAL_NAND_NUM; idx++)
    {
        FILE* fptr;
        snprintf(nand_name, 100, "%s/nand_%d", NAND_LOCATION, idx);
        fptr = fopen(nand_name, "w");
        if (fptr == NULL)
        {
            printf("open fail");
        }
        fclose(fptr);
    }

    // Start FUSE file system
    return fuse_main(argc, argv, &ssd_oper, NULL);
}
