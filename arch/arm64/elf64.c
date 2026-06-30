/* arch/arm64/elf64.c - minimal, bounds-checked ELF64 reader (Issue #24, M5) */

#include "elf64.h"

/* Return a pointer to `n` bytes at `off` in the image, or NULL if that range
 * does not lie wholly within [0, len). */
static const void *at(const void *img, size_t len, uint64_t off, uint64_t n)
{
    if (off > len || n > len || off + n > len)
        return (const void *)0;
    return (const unsigned char *)img + off;
}

int elf64_validate(const void *img, size_t len)
{
    const Elf64_Ehdr *e = at(img, len, 0, sizeof(*e));
    if (!e)
        return -1;
    if (e->e_ident[0] != 0x7f || e->e_ident[1] != 'E' ||
        e->e_ident[2] != 'L'  || e->e_ident[3] != 'F')
        return -2;
    if (e->e_ident[EI_CLASS] != ELFCLASS64)
        return -3;
    if (e->e_ident[EI_DATA] != ELFDATA2LSB)
        return -4;
    if (e->e_machine != EM_AARCH64)
        return -5;
    if (e->e_type != ET_EXEC && e->e_type != ET_DYN)
        return -6;
    if (e->e_phentsize != sizeof(Elf64_Phdr) || e->e_phnum == 0)
        return -7;
    /* The whole program-header table must be in bounds. */
    if (!at(img, len, e->e_phoff, (uint64_t)e->e_phnum * sizeof(Elf64_Phdr)))
        return -8;
    return 0;
}

uint16_t elf64_phnum(const void *img, size_t len)
{
    const Elf64_Ehdr *e = at(img, len, 0, sizeof(*e));
    return e ? e->e_phnum : 0;
}

const Elf64_Phdr *elf64_phdr(const void *img, size_t len, uint16_t i)
{
    const Elf64_Ehdr *e = at(img, len, 0, sizeof(*e));
    if (!e || i >= e->e_phnum)
        return (const Elf64_Phdr *)0;
    return at(img, len, e->e_phoff + (uint64_t)i * sizeof(Elf64_Phdr),
              sizeof(Elf64_Phdr));
}

uint64_t elf64_entry(const void *img)
{
    return ((const Elf64_Ehdr *)img)->e_entry;
}

/* Locate the section header at index i. */
static const Elf64_Shdr *shdr(const void *img, size_t len, uint16_t i)
{
    const Elf64_Ehdr *e = at(img, len, 0, sizeof(*e));
    if (!e || e->e_shentsize != sizeof(Elf64_Shdr) || i >= e->e_shnum)
        return (const Elf64_Shdr *)0;
    return at(img, len, e->e_shoff + (uint64_t)i * sizeof(Elf64_Shdr),
              sizeof(Elf64_Shdr));
}

/* Find the symbol table and its string table. */
static const Elf64_Shdr *find_symtab(const void *img, size_t len,
                                     const char **strtab, uint64_t *strsz)
{
    const Elf64_Ehdr *e = at(img, len, 0, sizeof(*e));
    if (!e)
        return (const Elf64_Shdr *)0;
    for (uint16_t i = 0; i < e->e_shnum; i++) {
        const Elf64_Shdr *s = shdr(img, len, i);
        if (!s || s->sh_type != SHT_SYMTAB)
            continue;
        const Elf64_Shdr *str = shdr(img, len, (uint16_t)s->sh_link);
        if (!str || str->sh_type != SHT_STRTAB)
            continue;
        const char *sp = at(img, len, str->sh_offset, str->sh_size);
        if (!sp || s->sh_entsize != sizeof(Elf64_Sym))
            continue;
        if (!at(img, len, s->sh_offset, s->sh_size))
            continue;
        *strtab = sp;
        *strsz  = str->sh_size;
        return s;
    }
    return (const Elf64_Shdr *)0;
}

static int streq_bounded(const char *a, const char *b, uint64_t amax)
{
    uint64_t i = 0;
    for (;; i++) {
        if (i >= amax)
            return 0;             /* ran off the string table */
        if (a[i] != b[i])
            return 0;
        if (a[i] == '\0')
            return 1;
    }
}

int elf64_symval(const void *img, size_t len, const char *name, uint64_t *value)
{
    const char *strtab; uint64_t strsz;
    const Elf64_Shdr *sym = find_symtab(img, len, &strtab, &strsz);
    if (!sym)
        return 0;

    uint64_t count = sym->sh_size / sizeof(Elf64_Sym);
    const Elf64_Sym *st = at(img, len, sym->sh_offset, sym->sh_size);
    if (!st)
        return 0;

    for (uint64_t i = 0; i < count; i++) {
        if (st[i].st_name == 0 || st[i].st_shndx == SHN_UNDEF)
            continue;
        if (st[i].st_name >= strsz)
            continue;
        if (streq_bounded(strtab + st[i].st_name, name, strsz - st[i].st_name)) {
            *value = st[i].st_value;
            return 1;
        }
    }
    return 0;
}

int elf64_undefined_count(const void *img, size_t len)
{
    const char *strtab; uint64_t strsz;
    const Elf64_Shdr *sym = find_symtab(img, len, &strtab, &strsz);
    if (!sym)
        return 0;

    uint64_t count = sym->sh_size / sizeof(Elf64_Sym);
    const Elf64_Sym *st = at(img, len, sym->sh_offset, sym->sh_size);
    if (!st)
        return 0;

    int n = 0;
    for (uint64_t i = 0; i < count; i++)
        if (st[i].st_shndx == SHN_UNDEF && st[i].st_name != 0 &&
            st[i].st_name < strsz && strtab[st[i].st_name] != '\0')
            n++;
    return n;
}

int elf64_disallowed_imports(const void *img, size_t len,
                             const char *const *allowed, int n_allowed)
{
    const char *strtab; uint64_t strsz;
    const Elf64_Shdr *sym = find_symtab(img, len, &strtab, &strsz);
    if (!sym)
        return 0;

    uint64_t count = sym->sh_size / sizeof(Elf64_Sym);
    const Elf64_Sym *st = at(img, len, sym->sh_offset, sym->sh_size);
    if (!st)
        return 0;

    int bad = 0;
    for (uint64_t i = 0; i < count; i++) {
        if (st[i].st_shndx != SHN_UNDEF || st[i].st_name == 0 ||
            st[i].st_name >= strsz || strtab[st[i].st_name] == '\0')
            continue;
        const char *name = strtab + st[i].st_name;
        int ok = 0;
        for (int a = 0; a < n_allowed && !ok; a++)
            ok = streq_bounded(name, allowed[a], strsz - st[i].st_name);
        if (!ok)
            bad++;
    }
    return bad;
}
