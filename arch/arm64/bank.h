/* arch/arm64/bank.h - patch banks + MIDI Program Change -> patch load
 * (Theme E, issue #122)
 *
 * A live rig switches patches from the floor, so Tessera maps MIDI Program
 * Change (and Bank Select) onto stored patches: banks of patches on the SD card,
 * each program in a bank a patch file the host loads.  This module is the pure
 * routing model - populate it by scanning the card, then feed it MIDI events and
 * it tells the host which patch file to load.  The actual load is patch_mgr's
 * job (reload the graph from the file, issue #40).
 *
 * MIDI bank selection follows the convention: CC 0 (Bank MSB) and CC 32 (Bank
 * LSB) latch a pending bank number (MSB*128 + LSB); the next Program Change
 * selects the program within that bank and resolves to a patch file.
 *
 * Pure, integer-only, host-tested (make test-arm-bank); no allocation, no libc.
 */

#ifndef ARM64_BANK_H
#define ARM64_BANK_H

#include "midi.h"
#include "patch.h"      /* PATCH_PATH_MAX */
#include <stdint.h>

#define BANK_MAX          8    /* banks (folders) on the card       */
#define BANK_PROGRAMS   128    /* programs per bank (MIDI PC range) */
#define BANK_NAME_MAX    16

typedef struct {
    char path[PATCH_PATH_MAX];   /* patch file for this program, "" if empty */
} bank_program_t;

typedef struct {
    char           name[BANK_NAME_MAX + 1];
    bank_program_t programs[BANK_PROGRAMS];
    int            n_programs;    /* highest populated program + 1 */
} bank_t;

typedef struct {
    bank_t   banks[BANK_MAX];
    int      n_banks;
    int      cur_bank;            /* bank a PC currently resolves against */
    uint8_t  pending_msb;         /* latched Bank Select MSB (CC 0)  */
    uint8_t  pending_lsb;         /* latched Bank Select LSB (CC 32) */
    int      have_pending;        /* a Bank Select arrived since last PC */
} bank_set_t;

void bank_set_init(bank_set_t *bs);

/* Add a bank by name.  Returns its index, or -1 if full. */
int  bank_add(bank_set_t *bs, const char *name);

/* Assign a patch file to (bank, program).  Returns 0 on success, -1 on a bad
 * index. */
int  bank_set_program(bank_set_t *bs, int bank, int program, const char *path);

/* Resolve (bank, program) to a patch path.  Returns 1 and writes `out_path`
 * (capacity PATCH_PATH_MAX) if that slot holds a patch, else 0. */
int  bank_resolve(const bank_set_t *bs, int bank, int program, char *out_path);

/* Feed a MIDI event.  A Bank Select CC latches the pending bank; a Program
 * Change selects the program in the current bank and, if that slot holds a
 * patch, writes its path to `out_path` and returns 1 (the host should load it).
 * Any other event, or an empty slot, returns 0. */
int  bank_midi(bank_set_t *bs, const midi_event_t *ev, char *out_path);

#endif /* ARM64_BANK_H */
