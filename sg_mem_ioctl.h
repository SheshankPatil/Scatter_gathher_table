/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SG_MEM_IOCTL_H_
#define _SG_MEM_IOCTL_H_

#include <linux/ioctl.h>
#define SG_MEM_MAGIC 's'
#define SG_MEM_SYNC_FOR_DEVICE _IO(SG_MEM_MAGIC,1)
#define SG_MEM_SYNC_FOR_CPU _IO(SG_MEM_MAGIC,2)

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint64_t __u64;
typedef uint32_t __u32;
typedef uint8_t __u8;
#endif

#define MAX_DESCRIPTORS 4
#define BURST_MULTIPLIER 16/* burst_length * 16 = one transaction size */

/*
 * DMA Descriptor
 *
 * burst_length: largest power-of-2 such that (burst_length * 16) divides length
 * evenly num_transactions: length / (burst_length * 16)
 *
 * Example for 6K (6144 bytes):
 *   Try 256: 256*16=4096, 6144/4096 = 1.5  -> NO
 *   Try 128: 128*16=2048, 6144/2048 = 3    -> YES
 *   burst_length=128, num_transactions=3
 *
 * Example for 4K (4096 bytes):
 *   Try 256: 256*16=4096, 4096/4096 = 1    -> YES
 *   burst_length=256, num_transactions=1
 */
struct dma_descriptor {
  __u64 phys_addr;        /* DMA address of the buffer */
  __u32 length;           /* total length of this buffer in bytes */
  __u32 burst_length;     /* bytes per burst beat (power of 2) */
  __u32 num_transactions; /* length / (burst_length * 16) */
  __u8 eof;               /* end of frame (1 = last descriptor) */
  __u8 reserved[3];
};

struct descriptor_table {
  __u32 num_descriptors;
  __u32 total_length;
  __u32 page_size;
  __u32 reserved;
  struct dma_descriptor desc[MAX_DESCRIPTORS];
};

#endif