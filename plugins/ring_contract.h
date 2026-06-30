/* plugins/ring_contract.h - shared layout for the audio-ring example plugins
 * (Issue #25, M5).
 *
 * The host maps the shared ring at RING_VA in the plugin's address space; the
 * plugin reads/writes it directly there.  Both the host harness and the
 * example plugins include this header so they agree on the address and the
 * block geometry (48 kHz / 256-frame blocks, float32 stereo).
 */

#ifndef TESSERA_RING_CONTRACT_H
#define TESSERA_RING_CONTRACT_H

/* User-space VAs of the shared rings, above the plugin's segments/stack/
 * trampoline/param pages (see plugin_loader.h).  RING_VA is a node's OUTPUT
 * ring; RING_IN_VA is its INPUT ring (used by effect-style nodes, issue #27). */
#define RING_VA      (0x8000000000ull + 0x0B000000ull)
#define RING_IN_VA   (0x8000000000ull + 0x0C000000ull)

#define RING_SR      48000u    /* sample rate                 */
#define RING_BLOCK   256u      /* frames per process block    */
#define RING_NBLOCKS 8u        /* blocks the producer writes  */
#define RING_FRAMES  (RING_BLOCK * RING_NBLOCKS)   /* ring capacity (pow2) */

#endif /* TESSERA_RING_CONTRACT_H */
