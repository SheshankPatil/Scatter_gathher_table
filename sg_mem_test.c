/*
 * sg_mem_test.c - Read DMA descriptor table, mmap memory, and test devmem/ioctls
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>      // Added for time()

#include "sg_mem_ioctl.h"

#define DEVICE_PATH "/dev/sg_mem"

/* * Helper function to calculate the exact physical address 
 * corresponding to a specific virtual offset in our scattered memory.
 */
uint64_t get_phys_addr(struct descriptor_table *dt, uint32_t target_offset) {
    uint32_t current_offset = 0;
    for (int i = 0; i < dt->num_descriptors; i++) {
        uint32_t chunk_len = dt->desc[i].length;
        if (target_offset >= current_offset && target_offset < current_offset + chunk_len) {
            // Found the right chunk! Add the remainder to the chunk's base physical address
            return dt->desc[i].phys_addr + (target_offset - current_offset);
        }
        current_offset += chunk_len;
    }
    return 0; // Out of bounds
}

int main(void)
{
    int fd;
    struct descriptor_table dt;
    ssize_t n;
    uint32_t i;
    uint8_t *mapped_buf;
    size_t map_size;

    srand(time(NULL));

    fd = open(DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        perror("Failed to open " DEVICE_PATH);
        return EXIT_FAILURE;
    }

    /* -------------------------------------------------------------
     * 1. GET SIZE AND PRINT DEVMEM HELPERS
     * ------------------------------------------------------------- */
    n = read(fd, &dt, sizeof(dt));
    if (n < 0) {
        perror("Failed to read descriptor table");
        close(fd);
        return EXIT_FAILURE;
    }
    
    map_size = dt.total_length;
    printf("========================================================\n");
    printf("Driver allocated: %zu bytes (%u chunks)\n", map_size, dt.num_descriptors);
    printf("========================================================\n\n");

    uint32_t current_virtual_offset = 0;
    for (i = 0; i < dt.num_descriptors && i < MAX_DESCRIPTORS; i++) {
        struct dma_descriptor *d = &dt.desc[i];
        printf("  Chunk [%u]:\n", i);
        printf("    Physical Base : 0x%016llx\n", (unsigned long long)d->phys_addr);
        printf("    Virtual Offset: +0x%X bytes\n", current_virtual_offset);
        printf("    Length        : %u bytes\n", d->length);
        printf("    -> Base devmem: sudo devmem 0x%llx\n\n", (unsigned long long)d->phys_addr);
        current_virtual_offset += d->length;
    }

    /* -------------------------------------------------------------
     * 2. MAP MEMORY
     * ------------------------------------------------------------- */
    mapped_buf = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped_buf == MAP_FAILED) {
        perror("mmap failed");
        close(fd);
        return EXIT_FAILURE;
    }
    printf("[+] Mapped at virtual address: %p\n", mapped_buf);

    /* -------------------------------------------------------------
     * 3. RANDOM ACCESS TEST WITH EXACT DEVMEM TRACKING
     * ------------------------------------------------------------- */
    printf("\n[+] Starting Random Access Test...\n");
    
    for (int k = 0; k < 5; k++) {
        int random_idx = rand() % map_size;
        uint8_t random_val = rand() % 255;
        uint64_t exact_phys = get_phys_addr(&dt, random_idx);

        printf("\n  [*] Writing 0x%02X to virtual offset [%d]\n", random_val, random_idx);
        
        // Write the data
        mapped_buf[random_idx] = random_val;
        
        // Read it back
        uint8_t read_back = mapped_buf[random_idx];

        if (read_back == random_val) {
            printf("      Status: OK! (Virt Addr: %p)\n", &mapped_buf[random_idx]);
            // Notice the '8' at the end of devmem. This tells devmem we are looking for an 8-bit (1 byte) value.
            printf("      -> Verify with: sudo devmem 0x%llx 8\n", (unsigned long long)exact_phys);
        } else {
            printf("      Status: FAIL! Read 0x%02X\n", read_back);
        }
    }

    /* -------------------------------------------------------------
     * 4. SYNC AND HOLD
     * ------------------------------------------------------------- */
    printf("\n[+] Triggering Syncs (Check dmesg!)...\n");
    ioctl(fd, SG_MEM_SYNC_FOR_DEVICE);
    ioctl(fd, SG_MEM_SYNC_FOR_CPU);

    printf("\n========================================================\n");
    printf("[!] MEMORY IS HELD OPEN.\n");
    printf("[!] Open a second terminal now and copy/paste the 'Verify with: sudo devmem...' commands above to see your data in physical RAM.\n");
    printf("========================================================\n");
    printf("\nPress ENTER to unmap memory and exit...");
    
    // Clear any leftover newline from stdin, then wait for user input
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
    getchar(); 

    /* -------------------------------------------------------------
     * 5. CLEANUP
     * ------------------------------------------------------------- */
    munmap(mapped_buf, map_size);
    close(fd);
    printf("Cleaned up and exited successfully.\n");
    
    return EXIT_SUCCESS;
}