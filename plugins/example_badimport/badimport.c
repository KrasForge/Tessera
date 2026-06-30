/* plugins/example_badimport/badimport.c - disallowed-import plugin (Issue #34)
 *
 * A plugin that pulls in a symbol from outside the plugin ABI (here a stand-in
 * for libc / a kernel symbol).  The loader walks the symbol table and rejects
 * any plugin with an undefined import that is not on the allowed list
 * (PM_EIMPORT) before mapping or running it.  Linked with
 * --unresolved-symbols=ignore-all so the undefined reference survives into the
 * binary's symbol table instead of failing the link.
 */

#include "plugin_abi.h"

/* Not provided by the plugin ABI: an illegal external dependency.  Declared as
 * data and referenced by address (a GOT/ABS64 relocation, which the linker can
 * leave undefined) rather than called (a PC-relative branch, which would
 * overflow when unresolved) so the binary keeps the UND symbol for the loader
 * to catch. */
extern volatile uint32_t forbidden_external_symbol;

uint32_t plugin_abi_version(void)
{
    return TESSERA_PLUGIN_ABI_VERSION;
}

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    /* Reference the forbidden import so it is emitted as an UND symbol. */
    volatile uint32_t *p = &forbidden_external_symbol;
    return (int)(uintptr_t)p ^ (int)(sample_rate ^ block_size);
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    (void)in_l; (void)in_r; (void)out_l; (void)out_r; (void)n_frames;
}

void plugin_set_param(uint32_t param_id, float value)
{
    (void)param_id; (void)value;
}

void plugin_destroy(void)
{
}
