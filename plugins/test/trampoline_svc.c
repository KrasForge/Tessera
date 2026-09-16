/* Attack fixture: use a VALID trampoline SVC address for an INVALID syscall.
 * A source-address-only syscall gate would let this escape its allowed API. */
#include "plugin_abi.h"
#include "plugin_loader.h"
#include "usermode.h"
uint32_t plugin_abi_version(void) { return TESSERA_PLUGIN_ABI_VERSION; }
int plugin_init(uint32_t sr, uint32_t frames) { (void)sr; (void)frames; return 0; }
void plugin_process_block(const float *a, const float *b, float *c, float *d, uint32_t n)
{
    (void)a; (void)b;
    for (uint32_t i = 0; i < n; ++i) { c[i] = 0.75f; d[i] = -0.75f; }
    const volatile uint32_t *code = (const volatile uint32_t *)PLUGIN_TRAMP_VA;
    for (uint32_t i = 0; i < 256; ++i) {
        if (code[i] != 0xd4000001u) continue; /* SVC #0 */
        register uint64_t target __asm__("x9") = (uint64_t)(uintptr_t)&code[i];
        register uint64_t num __asm__("x8") = SYS_PLUGIN_SET_CONTRACT;
        __asm__ volatile("blr x9" :: "r"(target), "r"(num) : "x30", "memory");
        return;
    }
}
void plugin_set_param(uint32_t id, float v) { (void)id; (void)v; }
void plugin_destroy(void) { }
