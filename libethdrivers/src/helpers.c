/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(NICTA_GPL)
 */

#include <camkes/dma.h>
#include <ethdrivers/helpers.h>
#include <sel4utils/iommu_dma.h>
#include <utils/arith.h>

dma_addr_t
dma_alloc_pin(ps_dma_man_t *dma_man, size_t size, int cached, int alignment)
{
    size = ROUND_UP(size, 4096);
    alignment = ROUND_UP(alignment, 4096);
    printf("[%s, %d] %x\n", __func__, __LINE__, size);
    void *virt = camkes_dma_alloc(size, alignment);
    printf("[%s, %d] %p\n", __func__, __LINE__, virt);
    //void *virt = ps_dma_alloc(dma_man, size, alignment, cached, PS_MEM_NORMAL);
    if (!virt) {
        return (dma_addr_t) {0, 0};
    }
    int error = sel4utils_iommu_dma_alloc_iospace(dma_man, virt, size);
    if (error) {
        printf("\nwtf\n");
        while(1);
    }
    uintptr_t phys = ps_dma_pin(dma_man, virt, size);
    if (!phys) {
        /* hmm this shouldn't really happen */
        ps_dma_free(dma_man, virt, size);
        printf("\nwtf\n");
        while(1);
        return (dma_addr_t) {0, 0};
    }
    if (!cached) {
        /* Prevent any cache bombs */
        ps_dma_cache_clean_invalidate(dma_man, virt, size);
    }
    return (dma_addr_t) {.virt = virt, .phys = phys};
}

void
dma_unpin_free(ps_dma_man_t *dma_man, void *virt, size_t size)
{
    ps_dma_unpin(dma_man, virt, size);
    ps_dma_free(dma_man, virt, size);
}
