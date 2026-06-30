/* plugins/example_echo/echo.c - parameter-echo plugin (Issue #30, M7)
 *
 * Demonstrates sys_plugin_set_param delivery: it drains its lock-free parameter
 * queue (mapped by the manager at PARAM_Q_VA) and records the last (id, value)
 * it received into a results page, so the host can confirm the parameter
 * arrived within one block.
 */

#include "plugin_abi.h"
#include "param_queue.h"
#include "ring_contract.h"

uint32_t plugin_abi_version(void) { return TESSERA_PLUGIN_ABI_VERSION; }

int plugin_init(uint32_t sample_rate, uint32_t block_size)
{
    (void)sample_rate;
    (void)block_size;

    param_queue_t   *q   = (param_queue_t *)PARAM_Q_VA;
    volatile uint32_t *res = (volatile uint32_t *)RESULTS_VA;

    uint32_t id = 0, bits = 0, n = 0;
    while (pq_pop(q, &id, &bits)) {       /* drain all pending params */
        res[1] = id;                       /* last id received   */
        res[2] = bits;                     /* last value (bits)  */
        n++;
    }
    res[0] = n;                            /* events drained      */
    return TESSERA_PLUGIN_OK;
}

void plugin_process_block(const float *in_l, const float *in_r,
                          float *out_l, float *out_r, uint32_t n_frames)
{
    for (uint32_t i = 0; i < n_frames; i++) { out_l[i] = in_l[i]; out_r[i] = in_r[i]; }
}

void plugin_set_param(uint32_t param_id, float value) { (void)param_id; (void)value; }
void plugin_destroy(void) { }
