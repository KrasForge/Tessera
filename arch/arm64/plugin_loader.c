/* arch/arm64/plugin_loader.c - load a plugin ELF into an isolated address
 *                              space (Issue #24, M5) */

#include "plugin_loader.h"
#include "elf64.h"
#include "process.h"
#include "vmem.h"
#include "pmm.h"
#include <stdint.h>
#include <stddef.h>

void *memcpy(void *, const void *, size_t);

/* The EL0 entry trampoline (plugin_trampoline.S). */
extern char plugin_tramp_start[], plugin_tramp_end[];

static unsigned perms_from_pflags(uint32_t pf)
{
    unsigned f = VMM_READ;
    if (pf & PF_W) f |= VMM_WRITE;
    if (pf & PF_X) f |= VMM_EXEC;
    return f;
}

/* Map one PT_LOAD segment page by page: allocate a zeroed frame per page, copy
 * the file-backed bytes that fall in it, and map it with the segment's perms.
 * Returns 0 on success. */
static int map_segment(plugin_t *pl, const unsigned char *elf, size_t len,
                       const Elf64_Phdr *ph)
{
    uint64_t seg_va  = ph->p_vaddr;
    uint64_t file_off = ph->p_offset;
    uint64_t filesz  = ph->p_filesz;
    uint64_t memsz   = ph->p_memsz;
    uint64_t end     = seg_va + memsz;
    unsigned flags   = perms_from_pflags(ph->p_flags);

    for (uint64_t page = seg_va & ~(PAGE_SIZE - 1); page < end; page += PAGE_SIZE) {
        uintptr_t pa = phys_alloc_page_zero();
        if (!pa)
            return PLUGIN_ENOMEM;

        /* File-backed bytes overlapping this page: [cs, ce). */
        uint64_t cs = page > seg_va ? page : seg_va;
        uint64_t file_end = seg_va + filesz;
        uint64_t ce = (page + PAGE_SIZE < file_end) ? page + PAGE_SIZE : file_end;
        if (ce > cs) {
            uint64_t n = ce - cs;
            uint64_t src = file_off + (cs - seg_va);
            if (src + n <= len)               /* bounds guard */
                memcpy((void *)(pa + (cs - page)),
                       elf + src, (size_t)n);
        }
        /* Tail of the page (incl. .bss) is already zero from the allocator. */

        if (process_map(pl->proc, pa, page, PAGE_SIZE, flags) != 0)
            return PLUGIN_ENOMEM;
    }
    return PLUGIN_OK;
}

int plugin_load(plugin_t *pl, const void *elf, size_t len, const char *name)
{
    const unsigned char *img = elf;

    for (size_t i = 0; i < sizeof(*pl); i++)
        ((unsigned char *)pl)[i] = 0;

    if (elf64_validate(elf, len) != 0)
        return PLUGIN_EBADELF;

    pl->proc = process_create(name);
    if (!pl->proc)
        return PLUGIN_ENOPROC;

    /* Map every PT_LOAD segment into the fresh address space. */
    uint16_t phnum = elf64_phnum(elf, len);
    for (uint16_t i = 0; i < phnum; i++) {
        const Elf64_Phdr *ph = elf64_phdr(elf, len, i);
        if (!ph || ph->p_type != PT_LOAD || ph->p_memsz == 0)
            continue;
        int r = map_segment(pl, img, len, ph);
        if (r != PLUGIN_OK)
            return r;
    }

    /* Resolve the ABI entry points.  plugin_init is mandatory. */
    if (!elf64_symval(elf, len, "plugin_init", &pl->init_va))
        return PLUGIN_ENOSYM;
    elf64_symval(elf, len, "plugin_abi_version",   &pl->abi_version_va);
    elf64_symval(elf, len, "plugin_process_block", &pl->process_va);
    elf64_symval(elf, len, "plugin_set_param",     &pl->setparam_va);
    elf64_symval(elf, len, "plugin_destroy",       &pl->destroy_va);

    /* Stack (RW). */
    uintptr_t stk = phys_alloc_page_zero();
    if (!stk || process_map(pl->proc, stk, PLUGIN_STACK_VA, PAGE_SIZE,
                            VMM_READ | VMM_WRITE) != 0)
        return PLUGIN_ENOMEM;
    pl->stack_top = PLUGIN_STACK_VA + PAGE_SIZE;

    /* Entry trampoline (RX): copy the host code into a plugin page. */
    uintptr_t tpa = phys_alloc_page_zero();
    if (!tpa)
        return PLUGIN_ENOMEM;
    size_t tlen = (size_t)(plugin_tramp_end - plugin_tramp_start);
    memcpy((void *)tpa, plugin_tramp_start, tlen);
    if (process_map(pl->proc, tpa, PLUGIN_TRAMP_VA, PAGE_SIZE,
                    VMM_READ | VMM_EXEC) != 0)
        return PLUGIN_ENOMEM;
    pl->entry_va = PLUGIN_TRAMP_VA;

    /* Parameter page (RW): kept also kernel-visible (identity PA) so the host
     * can write the call arguments before each entry. */
    uintptr_t ppa = phys_alloc_page_zero();
    if (!ppa || process_map(pl->proc, ppa, PLUGIN_PARAM_VA, PAGE_SIZE,
                            VMM_READ | VMM_WRITE) != 0)
        return PLUGIN_ENOMEM;
    pl->param_pa = ppa;
    pl->param_va = PLUGIN_PARAM_VA;

    return PLUGIN_OK;
}

int plugin_map_region(plugin_t *pl, uint64_t va, uintptr_t pa, size_t bytes,
                      unsigned flags)
{
    size_t mapped = (bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    return process_map(pl->proc, pa, va, mapped, flags);
}

long plugin_call_init(plugin_t *pl, uint32_t sample_rate, uint32_t block_size)
{
    /* Param block layout shared with plugin_trampoline.S. */
    *(volatile uint64_t *)(pl->param_pa + 0) = pl->init_va;
    *(volatile uint32_t *)(pl->param_pa + 8) = sample_rate;
    *(volatile uint32_t *)(pl->param_pa + 12) = block_size;

    return process_run(pl->proc, pl->entry_va, pl->stack_top, pl->param_va);
}

long plugin_call_abi_version(plugin_t *pl)
{
    if (!pl->abi_version_va)
        return -1;
    /* Reuse the trampoline: call abi_version (it ignores the rate/size args)
     * and exit with its return value. */
    *(volatile uint64_t *)(pl->param_pa + 0) = pl->abi_version_va;
    *(volatile uint32_t *)(pl->param_pa + 8) = 0;
    *(volatile uint32_t *)(pl->param_pa + 12) = 0;

    return process_run(pl->proc, pl->entry_va, pl->stack_top, pl->param_va);
}
