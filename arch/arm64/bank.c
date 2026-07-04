/* arch/arm64/bank.c - patch banks + MIDI Program Change -> patch load
 * (Theme E, issue #122).  See bank.h. */

#include "bank.h"

/* MIDI Bank Select controllers. */
#define CC_BANK_MSB  0u
#define CC_BANK_LSB 32u

static void str_copy(char *dst, const char *src, int cap)
{
    int i = 0;
    for (; src && src[i] && i < cap; i++) dst[i] = src[i];
    dst[i] = '\0';
}

void bank_set_init(bank_set_t *bs)
{
    bs->n_banks      = 0;
    bs->cur_bank     = 0;
    bs->pending_msb  = 0;
    bs->pending_lsb  = 0;
    bs->have_pending = 0;
    for (int b = 0; b < BANK_MAX; b++) {
        bs->banks[b].name[0]    = '\0';
        bs->banks[b].n_programs = 0;
        for (int p = 0; p < BANK_PROGRAMS; p++)
            bs->banks[b].programs[p].path[0] = '\0';
    }
}

int bank_add(bank_set_t *bs, const char *name)
{
    if (bs->n_banks >= BANK_MAX)
        return -1;
    int i = bs->n_banks++;
    str_copy(bs->banks[i].name, name, BANK_NAME_MAX);
    return i;
}

int bank_set_program(bank_set_t *bs, int bank, int program, const char *path)
{
    if (bank < 0 || bank >= bs->n_banks || program < 0 || program >= BANK_PROGRAMS)
        return -1;
    str_copy(bs->banks[bank].programs[program].path, path, PATCH_PATH_MAX - 1);
    if (program + 1 > bs->banks[bank].n_programs)
        bs->banks[bank].n_programs = program + 1;
    return 0;
}

int bank_resolve(const bank_set_t *bs, int bank, int program, char *out_path)
{
    if (bank < 0 || bank >= bs->n_banks || program < 0 || program >= BANK_PROGRAMS)
        return 0;
    const char *p = bs->banks[bank].programs[program].path;
    if (p[0] == '\0')
        return 0;                    /* empty slot */
    str_copy(out_path, p, PATCH_PATH_MAX - 1);
    return 1;
}

int bank_midi(bank_set_t *bs, const midi_event_t *ev, char *out_path)
{
    if (ev->type == MIDI_CC) {
        if (ev->data1 == CC_BANK_MSB) { bs->pending_msb = ev->data2; bs->have_pending = 1; }
        else if (ev->data1 == CC_BANK_LSB) { bs->pending_lsb = ev->data2; bs->have_pending = 1; }
        return 0;                    /* Bank Select only latches; PC commits it */
    }

    if (ev->type == MIDI_PROGRAM) {
        /* Commit any latched Bank Select first. */
        if (bs->have_pending) {
            int bank = (int)bs->pending_msb * 128 + (int)bs->pending_lsb;
            if (bank >= 0 && bank < bs->n_banks)
                bs->cur_bank = bank;
            /* An out-of-range bank leaves cur_bank unchanged (ignored). */
            bs->have_pending = 0;
        }
        return bank_resolve(bs, bs->cur_bank, ev->data1, out_path);
    }

    return 0;
}
