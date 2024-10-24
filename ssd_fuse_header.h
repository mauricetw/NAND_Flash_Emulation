/*
  FUSE-ioctl: ioctl support for FUSE
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>
  This program can be distributed under the terms of the GNU GPLv2.
  See the file COPYING.
*/
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <limits.h>
#define PHYSICAL_NAND_NUM (8)
#define LOGICAL_NAND_NUM (5)
#define NAND_SIZE_KB (10)
#define INVALID_PCA  (0xFFFFFFFFU)
#define FULL_PCA     (0xFFFFFFFFU)
#define INVALID_LBA (0xFFFFFFFFU)
#define PAGES_PER_BLOCK (NAND_SIZE_KB * 1024 / 512)
#define NAND_LOCATION  "/home/stanwang/Desktop/NAND_Flash_Emulation/nand"

enum
{
    SSD_GET_LOGIC_SIZE   = _IOR('E', 0, size_t),
    SSD_GET_PHYSIC_SIZE   = _IOR('E', 1, size_t),
    SSD_GET_WA            = _IOR('E', 2, size_t),
};
