/* arch/arm64/elf64.h - minimal, bounds-checked ELF64 reader (Issue #24, M5)
 *
 * The ARM port of the IKOS ELF loader (kernel/elf_loader.c).  Just enough of
 * the ELF64 format to load an AArch64 plugin: validate the header, walk the
 * PT_LOAD program headers, and resolve a symbol by name from the symbol table.
 * Pure and bounds-checked against the image length, so it is unit-tested on the
 * host with synthetic images (make test-arm-elf).
 */

#ifndef ARM64_ELF64_H
#define ARM64_ELF64_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} Elf64_Shdr;

typedef struct {
    uint32_t st_name;
    unsigned char st_info;
    unsigned char st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

/* e_ident indices and values. */
#define EI_CLASS     4
#define EI_DATA      5
#define ELFCLASS64   2
#define ELFDATA2LSB  1

#define ET_EXEC      2
#define ET_DYN       3
#define EM_AARCH64   183

#define PT_LOAD      1
#define PF_X         0x1
#define PF_W         0x2
#define PF_R         0x4

#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHN_UNDEF    0

/* Validate that `img`/`len` is a little-endian AArch64 ELF64 executable.
 * Returns 0 on success, a negative code otherwise. */
int elf64_validate(const void *img, size_t len);

/* Program-header access (bounds-checked). */
uint16_t          elf64_phnum(const void *img, size_t len);
const Elf64_Phdr *elf64_phdr(const void *img, size_t len, uint16_t i);

/* Entry point VA from the header. */
uint64_t elf64_entry(const void *img);

/* Resolve a symbol by name to its value (VA).  Returns 1 and writes *value on
 * success, 0 if the symbol is absent or the tables are malformed. */
int elf64_symval(const void *img, size_t len, const char *name, uint64_t *value);

/* Count defined-elsewhere (SHN_UNDEF) named symbols, i.e. unresolved imports.
 * A self-contained plugin has zero. */
int elf64_undefined_count(const void *img, size_t len);

/* Count undefined (imported) named symbols that are NOT in the `allowed` list
 * of `n_allowed` names.  Zero means every import is permitted (a self-contained
 * plugin with no imports, or one importing only allowed symbols).  Used to
 * reject plugins that pull in libc or kernel symbols (issue #34). */
int elf64_disallowed_imports(const void *img, size_t len,
                             const char *const *allowed, int n_allowed);

#endif /* ARM64_ELF64_H */
