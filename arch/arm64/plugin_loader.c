/* arch/arm64/plugin_loader.c - load a plugin ELF into an isolated address
 *                              space (Issue #24, M5) */

#include "plugin_loader.h"
#include "elf64.h"
#include "process.h"
#include "vmem.h"
#include "pmm.h"
#include "sandbox.h"
#include "budget.h"
#include <stdint.h>
#include <stddef.h>

void *memcpy(void *, const void *, size_t);

/* The EL0 entry trampoline (plugin_trampoline.S). */
extern char plugin_tramp_start[], plugin_tramp_end[];

/* Record a mapped region so sandbox_audit() has an exact allowlist (#35). */
static void record_region(plugin_t *pl, uint64_t va, uint64_t len, unsigned flags)
{
    uint64_t start = va & ~(PAGE_SIZE - 1);
    uint64_t end   = (va + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (pl->n_regions < PLUGIN_MAX_REGIONS) {
        pl->regions[pl->n_regions].va    = start;
        pl->regions[pl->n_regions].len   = end - start;
        pl->regions[pl->n_regions].flags = flags;
        pl->n_regions++;
    }
}

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

        if (process_map(pl->proc, pa, page, PAGE_SIZE, flags) != 0) {
            phys_free_page(pa);
            return PLUGIN_ENOMEM;
        }
    }
    record_region(pl, seg_va, memsz, flags);
    return PLUGIN_OK;
}

static int load_failed(plugin_t *pl, int error)
{
    if (pl->proc) process_destroy(pl->proc);
    pl->proc = (process_t *)0;
    return error;
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
            return load_failed(pl, r);
    }

    /* Resolve the ABI entry points.  plugin_init is mandatory. */
    if (!elf64_symval(elf, len, "plugin_init", &pl->init_va))
        return load_failed(pl, PLUGIN_ENOSYM);
    elf64_symval(elf, len, "plugin_abi_version",   &pl->abi_version_va);
    elf64_symval(elf, len, "plugin_process_block", &pl->process_va);
    elf64_symval(elf, len, "plugin_set_param",     &pl->setparam_va);
    elf64_symval(elf, len, "plugin_destroy",       &pl->destroy_va);

    /* Stack (RW). */
    uintptr_t stk = phys_alloc_page_zero();
    if (!stk) return load_failed(pl, PLUGIN_ENOMEM);
    if (process_map(pl->proc, stk, PLUGIN_STACK_VA, PAGE_SIZE,
                    VMM_READ | VMM_WRITE) != 0) {
        phys_free_page(stk);
        return load_failed(pl, PLUGIN_ENOMEM);
    }
    pl->stack_top = PLUGIN_STACK_VA + PAGE_SIZE;
    record_region(pl, PLUGIN_STACK_VA, PAGE_SIZE, VMM_READ | VMM_WRITE);

    /* Entry trampoline (RX): copy the host code into a plugin page. */
    uintptr_t tpa = phys_alloc_page_zero();
    if (!tpa) return load_failed(pl, PLUGIN_ENOMEM);
    size_t tlen = (size_t)(plugin_tramp_end - plugin_tramp_start);
    memcpy((void *)tpa, plugin_tramp_start, tlen);
    if (process_map(pl->proc, tpa, PLUGIN_TRAMP_VA, PAGE_SIZE,
                    VMM_READ | VMM_EXEC) != 0) {
        phys_free_page(tpa);
        return load_failed(pl, PLUGIN_ENOMEM);
    }
    pl->entry_va = PLUGIN_TRAMP_VA;
    record_region(pl, PLUGIN_TRAMP_VA, PAGE_SIZE, VMM_READ | VMM_EXEC);

    /* Parameter page (RW): kept also kernel-visible (identity PA) so the host
     * can write the call arguments before each entry. */
    uintptr_t ppa = phys_alloc_page_zero();
    if (!ppa) return load_failed(pl, PLUGIN_ENOMEM);
    if (process_map(pl->proc, ppa, PLUGIN_PARAM_VA, PAGE_SIZE,
                    VMM_READ | VMM_WRITE) != 0) {
        phys_free_page(ppa);
        return load_failed(pl, PLUGIN_ENOMEM);
    }
    pl->param_pa = ppa;
    pl->param_va = PLUGIN_PARAM_VA;
    record_region(pl, PLUGIN_PARAM_VA, PAGE_SIZE, VMM_READ | VMM_WRITE);

    return PLUGIN_OK;
}

int plugin_map_region(plugin_t *pl, uint64_t va, uintptr_t pa, size_t bytes,
                      unsigned flags)
{
    size_t mapped = (bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    int rc = process_map(pl->proc, pa, va, mapped, flags);
    if (rc == 0)
        record_region(pl, va, mapped, flags);
    return rc;
}

int plugin_sandbox_regions(const plugin_t *pl, struct sandbox_region *out, int max)
{
    int n = 0;
    for (int i = 0; i < pl->n_regions && n < max; i++) {
        out[n].va  = pl->regions[i].va;
        out[n].len = pl->regions[i].len;
        n++;
    }
    return n;
}

/* Weak linkage preserves tiny legacy fixtures; requesting protection without
 * the timer implementation returns ENOTSUP rather than executing unbounded. */
extern long budget_call(long (*run)(void *), void *ctx, uint64_t cycles) __attribute__((weak));
static long enter_plugin(void *ctx)
{
    plugin_t *pl = ctx;
    return process_run(pl->proc, pl->entry_va, pl->stack_top, pl->param_va);
}

static long call_fn(plugin_t *pl, uint64_t fn, uint64_t a0, uint64_t a1,
                    uint64_t a2, uint64_t a3, uint64_t a4, uint64_t mode)
{
    if (!pl || !pl->proc || !fn || !pl->param_pa) return -1;
    uint32_t idle = 0;
    if (!__atomic_compare_exchange_n(&pl->call_busy, &idle, 1u, 0,
                                     __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return PLUGIN_EBUSY;
    volatile uint64_t *pb = (volatile uint64_t *)pl->param_pa;
    pb[0] = fn; pb[1] = a0; pb[2] = a1; pb[3] = a2;
    pb[4] = a3; pb[5] = a4; pb[6] = mode;
    long result;
    if (pl->lifecycle_ticks && fn != pl->process_va) {
        result = budget_call ? budget_call(enter_plugin, pl, pl->lifecycle_ticks) : PLUGIN_ENOTSUP;
        if (result == BUDGET_PREEMPTED) {
            process_kill(pl->proc, PLUGIN_ETIMEOUT);
            result = PLUGIN_ETIMEOUT;
        }
    } else result = enter_plugin(pl);
    __atomic_store_n(&pl->call_busy, 0u, __ATOMIC_RELEASE);
    return result;
}

long plugin_call_init(plugin_t *pl, uint32_t sample_rate, uint32_t block_size)
{
    if (!pl || __atomic_load_n(&pl->host_refs, __ATOMIC_ACQUIRE)) return PLUGIN_EBUSY;
    return call_fn(pl, pl->init_va, sample_rate, block_size, 0, 0, 0, 0);
}

long plugin_call_abi_version(plugin_t *pl)
{
    if (!pl->abi_version_va)
        return -1;
    return call_fn(pl, pl->abi_version_va, 0, 0, 0, 0, 0, 0);
}

long plugin_call_block(plugin_t *pl, uint64_t in_l, uint64_t in_r,
                       uint64_t out_l, uint64_t out_r, uint32_t n_frames)
{
    if (!pl->process_va)
        return -1;
    return call_fn(pl, pl->process_va, in_l, in_r, out_l, out_r, n_frames, 2);
}

long plugin_call_set_param(plugin_t *pl, uint32_t id, uint32_t value_bits)
{
    if (!pl || __atomic_load_n(&pl->host_refs, __ATOMIC_ACQUIRE)) return PLUGIN_EBUSY;
    return call_fn(pl, pl ? pl->setparam_va : 0, id, value_bits, 0, 0, 0, 3);
}

long plugin_call_destroy(plugin_t *pl)
{
    if (!pl || !pl->destroy_va) return 0;
    if (__atomic_load_n(&pl->host_refs, __ATOMIC_ACQUIRE)) return PLUGIN_EBUSY;
    return call_fn(pl, pl->destroy_va, 0, 0, 0, 0, 0, 2);
}
