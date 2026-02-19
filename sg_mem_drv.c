// SPDX-License-Identifier: GPL-2.0
/*
 * Dynamic scatter-gather memory driver with DMA descriptors
 * Accepts size from userspace via IOCTL and creates optimal page layout
 */
#include "sg_mem_ioctl.h"
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define DEVICE_NAME "sg_mem"

struct sg_mem_dev {
  struct page **pages; /* dynamic array */
  __u32 *chunk_sizes;  /* size of each chunk */
  __u32 num_chunks;    /* number of descriptors */
  __u32 total_size;    /* total allocated bytes */
  struct sg_table sgt;
  struct descriptor_table *dtable;
  bool allocated;
  bool dma_mapped;
  struct device *dev;
  struct mutex lock;
};


static int alloc_size = 16384; // Default 16KB
module_param(alloc_size, int, 0444); // 0444 = Read-only in sysfs
MODULE_PARM_DESC(alloc_size, "Size of memory to allocate in bytes");

/* Add this helper function to print the table internals */
static void print_sg_table_debug(struct sg_mem_dev *sdev) {
    struct scatterlist *sg;
    int i;
    
    pr_info("sg_mem: ==============================================\n");
    pr_info("sg_mem:        INTERNAL SCATTER-GATHER TABLE          \n");
    pr_info("sg_mem: ==============================================\n");
    
    for_each_sg(sdev->sgt.sgl, sg, sdev->sgt.orig_nents, i) {
        // We print:
        // 1. The loop index (Chunk #)
        // 2. The ACTUAL Physical Address in RAM (phys)
        // 3. The length of this specific chunk
        // 4. The DMA address (what the hardware sees)
        pr_info("sg_mem: Chunk [%d]: Phys=0x%llx | Len=%u | DMA_Addr=0x%llx\n", 
                i, 
                (u64)page_to_phys(sg_page(sg)), 
                sg->length,
                sg_dma_address(sg));
    }
    pr_info("sg_mem: ==============================================\n");
}
static struct sg_mem_dev sg_dev;
static struct platform_device *pdev;

static __u32 calc_burst_length(__u32 length) {
  __u32 burst;

  for (burst = 256; burst >= 1; burst >>= 1) {
    __u32 txn_size = burst * BURST_MULTIPLIER;
    if (length >= txn_size && (length % txn_size) == 0)
      return burst;
  }
  return 1; /* fallback */
}

static void build_descriptor_table(struct sg_mem_dev *sdev) {
  struct descriptor_table *dt = sdev->dtable;
  struct scatterlist *sg;
  unsigned int idx;
  __u32 total_len = 0;

  memset(dt, 0, sizeof(*dt));
  dt->page_size = PAGE_SIZE;

  idx = 0;
  for_each_sgtable_dma_sg(&sdev->sgt, sg, idx) {
    __u32 len, burst, txn_size;

    if (idx >= MAX_DESCRIPTORS)
      break;

    len = sg_dma_len(sg);
    burst = calc_burst_length(len);
    txn_size = burst * BURST_MULTIPLIER;

    dt->desc[idx].phys_addr = (__u64)sg_dma_address(sg);
    dt->desc[idx].length = len;
    dt->desc[idx].burst_length = burst;
    dt->desc[idx].num_transactions = len / txn_size;
    dt->desc[idx].eof = 0;

    total_len += len;
  }

  dt->num_descriptors = idx;
  dt->total_length = total_len;

  if (dt->num_descriptors > 0)
    dt->desc[dt->num_descriptors - 1].eof = 1;

  for (idx = 0; idx < dt->num_descriptors; idx++) {
    struct dma_descriptor *d = &dt->desc[idx];
    pr_info("sg_mem: desc[%u]: phys=0x%llx len=%u burst=%u txn_size=%u txns=%u "
            "eof=%u\n",
            idx, d->phys_addr, d->length, d->burst_length,
            d->burst_length * BURST_MULTIPLIER, d->num_transactions, d->eof);
  }
}

static int sg_mem_alloc(struct sg_mem_dev *sdev, __u32 size_bytes) {
  int ret, i;
  __u32 remaining, chunk_idx;
  struct scatterlist *sg;

  if (size_bytes == 0 || size_bytes > (MAX_DESCRIPTORS * PAGE_SIZE)) {
    pr_err("sg_mem: invalid size %u (max %lu)\n", size_bytes,
           MAX_DESCRIPTORS * PAGE_SIZE);
    return -EINVAL;
  }

  /* Calculate number of chunks */
  sdev->num_chunks = (size_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
  sdev->total_size = size_bytes;

  pr_info("sg_mem: Allocating %u bytes in %u chunks\n", size_bytes,
          sdev->num_chunks);

  /* Allocate arrays */
  sdev->pages = kzalloc(sizeof(struct page *) * sdev->num_chunks, GFP_KERNEL);
  if (!sdev->pages)
    return -ENOMEM;

  sdev->chunk_sizes = kzalloc(sizeof(__u32) * sdev->num_chunks, GFP_KERNEL);
  if (!sdev->chunk_sizes) {
    kfree(sdev->pages);
    sdev->pages = NULL;
    return -ENOMEM;
  }

  sdev->dtable = kzalloc(sizeof(*sdev->dtable), GFP_KERNEL);
  if (!sdev->dtable) {
    ret = -ENOMEM;
    goto err_free_arrays;
  }

  /* Allocate pages and determine chunk sizes */
  remaining = size_bytes;
  for (chunk_idx = 0; chunk_idx < sdev->num_chunks; chunk_idx++) {
    __u32 chunk_size;

    sdev->pages[chunk_idx] = alloc_page(GFP_KERNEL | __GFP_ZERO);
    if (!sdev->pages[chunk_idx]) {
      pr_err("sg_mem: failed to allocate page %u\n", chunk_idx);
      ret = -ENOMEM;
      goto err_free_pages;
    }

    /* Full PAGE_SIZE or remainder */
    chunk_size = (remaining >= PAGE_SIZE) ? PAGE_SIZE : remaining;
    sdev->chunk_sizes[chunk_idx] = chunk_size;
    remaining -= chunk_size;

    pr_info("sg_mem:   Chunk[%u]: phys=0x%llx size=%u bytes\n", chunk_idx,
            (u64)page_to_phys(sdev->pages[chunk_idx]), chunk_size);
  }

  /* Build SG table with custom chunk sizes */
  ret = sg_alloc_table(&sdev->sgt, sdev->num_chunks, GFP_KERNEL);
  if (ret) {
    pr_err("sg_mem: sg_alloc_table failed: %d\n", ret);
    goto err_free_pages;
  }

  sg = sdev->sgt.sgl;
  for (i = 0; i < sdev->num_chunks; i++) {
    sg_set_page(sg, sdev->pages[i], sdev->chunk_sizes[i], 0);
    sg = sg_next(sg);
  }

  /* Map for DMA */
  ret = dma_map_sgtable(sdev->dev, &sdev->sgt, DMA_BIDIRECTIONAL, 0);
  if (ret) {
    pr_err("sg_mem: dma_map_sgtable failed: %d\n", ret);
    goto err_free_sgt;
  }
  sdev->dma_mapped = true;

  build_descriptor_table(sdev);

  sdev->allocated = true;
  return 0;

err_free_sgt:
  sg_free_table(&sdev->sgt);
err_free_pages:
  for (i = 0; i < chunk_idx; i++)
    if (sdev->pages[i])
      __free_page(sdev->pages[i]);
  kfree(sdev->dtable);
  sdev->dtable = NULL;
err_free_arrays:
  kfree(sdev->chunk_sizes);
  kfree(sdev->pages);
  sdev->pages = NULL;
  sdev->chunk_sizes = NULL;
  return ret;
}

static void sg_mem_free(struct sg_mem_dev *sdev) {
  int i;
  if (!sdev->allocated)
    return;
  if (sdev->dma_mapped)
    dma_unmap_sgtable(sdev->dev, &sdev->sgt, DMA_BIDIRECTIONAL, 0);
  sg_free_table(&sdev->sgt);
  for (i = 0; i < sdev->num_chunks; i++)
    if (sdev->pages[i])
      __free_page(sdev->pages[i]);
  kfree(sdev->dtable);
  sdev->dtable = NULL;
  kfree(sdev->pages);
  kfree(sdev->chunk_sizes);
  sdev->pages = NULL;
  sdev->chunk_sizes = NULL;
  sdev->allocated = false;

}

static int sg_mem_open(struct inode *inode, struct file *filp) { return 0; }

static int sg_mem_release(struct inode *inode, struct file *filp) { return 0; }

static ssize_t sg_mem_read(struct file *filp, char __user *buf, size_t count,
                           loff_t *ppos) {
  if (!sg_dev.allocated || !sg_dev.dtable)
    return -ENODEV;

  return simple_read_from_buffer(buf, count, ppos, sg_dev.dtable,
                                 sizeof(*sg_dev.dtable));
}


static int sg_mem_mmap(struct file *filp, struct vm_area_struct *vma)
{
    unsigned long user_vaddr = vma->vm_start;
    struct scatterlist *sg;
    int i;

    if (!sg_dev.allocated)
        return -ENODEV;

    for_each_sg(sg_dev.sgt.sgl, sg, sg_dev.sgt.orig_nents, i) {
        struct page *page = sg_page(sg);
        int ret;

        ret = vm_insert_page(vma, user_vaddr, page);
        if (ret)
            return ret;

        user_vaddr += PAGE_SIZE;
    }

    return 0;
}



static long sg_mem_ioctl(struct file *filp, unsigned int cmd,
                         unsigned long arg) {
  if (!sg_dev.allocated)
    return -ENODEV;

  switch (cmd) {
  case SG_MEM_SYNC_FOR_DEVICE:

    dma_sync_sgtable_for_device(sg_dev.dev, &sg_dev.sgt, DMA_BIDIRECTIONAL);
    pr_info("sg_mem: Synced for Device (cache flushed)\n");
    break;

  case SG_MEM_SYNC_FOR_CPU:
    dma_sync_sgtable_for_cpu(sg_dev.dev, &sg_dev.sgt, DMA_BIDIRECTIONAL);
    pr_info("sg_mem: Synced for CPU (cache invalidated)\n");
    break;

  default:
    return -ENOTTY;
  }

  return 0;
}

static const struct file_operations sg_mem_fops = {
    .owner = THIS_MODULE,
    .open = sg_mem_open,
    .release = sg_mem_release,
    .read = sg_mem_read,
    .mmap = sg_mem_mmap,
    .unlocked_ioctl = sg_mem_ioctl,
};

static struct miscdevice sg_mem_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = DEVICE_NAME,
    .fops = &sg_mem_fops,
    .mode = 0666,
};

static int __init sg_mem_init(void) {
  int ret;

  pr_info("sg_mem: initializing with request size = %d bytes...\n", alloc_size);

  pdev = platform_device_register_simple("sg_mem_dma", -1, NULL, 0);
  if (IS_ERR(pdev))
    return PTR_ERR(pdev);

  dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));

  memset(&sg_dev, 0, sizeof(sg_dev));
  sg_dev.dev = &pdev->dev;

  // Pass the module parameter 'alloc_size' here!
  ret = sg_mem_alloc(&sg_dev, alloc_size);
  if (ret)
    goto err_pdev;

  // Call our new debug printer
  print_sg_table_debug(&sg_dev);

  ret = misc_register(&sg_mem_misc);
  if (ret) {
    sg_mem_free(&sg_dev);
    goto err_pdev;
  }

  pr_info("sg_mem: registered /dev/%s\n", DEVICE_NAME);
  return 0;

err_pdev:
  platform_device_unregister(pdev);
  return ret;
}

static void __exit sg_mem_exit(void) {
  misc_deregister(&sg_mem_misc);
  sg_mem_free(&sg_dev);
  platform_device_unregister(pdev);
  pr_info("sg_mem: unloaded\n");
}

module_init(sg_mem_init);
module_exit(sg_mem_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Demo");
MODULE_DESCRIPTION("SG non-contiguous memory with DMA descriptor table");