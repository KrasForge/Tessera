/* tests/arm64/elf_test.c - host unit tests for the ELF64 reader (Issue #24).
 *
 * Builds a minimal but valid AArch64 ELF64 image in memory (header + one
 * PT_LOAD program header + a symbol table with one defined symbol and one
 * undefined import) and checks validation, program-header walking, symbol
 * resolution, and the import count - plus rejection of malformed images.
 *
 * Build/run via:  make test-arm-elf
 */

#include "elf64.h"

#include <string.h>
#include <stdio.h>

static int g_fail;
#define CHECK(cond, msg) do {                                            \
        if (cond) { printf("  ok   : %s\n", msg); }                      \
        else      { printf("  FAIL : %s\n", msg); g_fail++; }            \
    } while (0)

#define PLUGIN_VA 0x8000000000ull

static uint8_t g_img[2048];
static size_t  g_len;

/* Assemble a tiny valid image; returns its length. */
static size_t build_elf(void)
{
    memset(g_img, 0, sizeof(g_img));
    size_t off = sizeof(Elf64_Ehdr);

    /* One PT_LOAD program header. */
    size_t phoff = off;
    Elf64_Phdr ph = {0};
    ph.p_type = PT_LOAD; ph.p_flags = PF_R | PF_X;
    ph.p_offset = 0; ph.p_vaddr = PLUGIN_VA; ph.p_paddr = PLUGIN_VA;
    ph.p_filesz = 0x40; ph.p_memsz = 0x80; ph.p_align = 0x1000;
    memcpy(g_img + phoff, &ph, sizeof(ph));
    off = phoff + sizeof(ph);

    /* String table: "\0plugin_init\0ext_import\0" */
    size_t stroff = off;
    const char *s0 = "";            (void)s0;
    size_t name_init = 1;
    memcpy(g_img + stroff + name_init, "plugin_init", 12);
    size_t name_imp = name_init + 12;
    memcpy(g_img + stroff + name_imp, "ext_import", 11);
    size_t strsz = name_imp + 11;
    off = stroff + strsz;
    off = (off + 7) & ~7ull;

    /* Symbol table: null, plugin_init (defined), ext_import (UNDEF). */
    size_t symoff = off;
    Elf64_Sym syms[3] = {0};
    syms[1].st_name = (uint32_t)name_init; syms[1].st_shndx = 1;
    syms[1].st_value = PLUGIN_VA + 0x10;
    syms[2].st_name = (uint32_t)name_imp;  syms[2].st_shndx = SHN_UNDEF;
    memcpy(g_img + symoff, syms, sizeof(syms));
    off = symoff + sizeof(syms);

    /* Section headers: null, symtab(link->strtab idx 2), strtab. */
    size_t shoff = off;
    Elf64_Shdr sh[3] = {0};
    sh[1].sh_type = SHT_SYMTAB; sh[1].sh_offset = symoff;
    sh[1].sh_size = sizeof(syms); sh[1].sh_link = 2;
    sh[1].sh_entsize = sizeof(Elf64_Sym);
    sh[2].sh_type = SHT_STRTAB; sh[2].sh_offset = stroff; sh[2].sh_size = strsz;
    memcpy(g_img + shoff, sh, sizeof(sh));
    off = shoff + sizeof(sh);

    /* ELF header. */
    Elf64_Ehdr e = {0};
    e.e_ident[0] = 0x7f; e.e_ident[1] = 'E'; e.e_ident[2] = 'L'; e.e_ident[3] = 'F';
    e.e_ident[EI_CLASS] = ELFCLASS64; e.e_ident[EI_DATA] = ELFDATA2LSB;
    e.e_type = ET_EXEC; e.e_machine = EM_AARCH64; e.e_version = 1;
    e.e_entry = PLUGIN_VA + 0x10;
    e.e_phoff = phoff; e.e_phentsize = sizeof(Elf64_Phdr); e.e_phnum = 1;
    e.e_shoff = shoff; e.e_shentsize = sizeof(Elf64_Shdr); e.e_shnum = 3;
    memcpy(g_img, &e, sizeof(e));

    return off;
}

int main(void)
{
    printf("=== Tessera ELF64-reader tests (issue #24) ===\n");
    g_len = build_elf();

    CHECK(elf64_validate(g_img, g_len) == 0, "valid AArch64 ELF accepted");
    CHECK(elf64_phnum(g_img, g_len) == 1, "one program header");

    const Elf64_Phdr *ph = elf64_phdr(g_img, g_len, 0);
    CHECK(ph && ph->p_type == PT_LOAD, "program header is PT_LOAD");
    CHECK(ph && ph->p_vaddr == PLUGIN_VA, "segment maps at the plugin base VA");
    CHECK(ph && ph->p_memsz > ph->p_filesz, "memsz > filesz (has .bss to zero)");

    uint64_t v = 0;
    CHECK(elf64_symval(g_img, g_len, "plugin_init", &v) == 1 && v == PLUGIN_VA + 0x10,
          "plugin_init resolves to its defined VA");
    CHECK(elf64_symval(g_img, g_len, "missing", &v) == 0, "absent symbol not resolved");
    CHECK(elf64_symval(g_img, g_len, "ext_import", &v) == 0,
          "UNDEF import is not resolved as a definition");
    CHECK(elf64_undefined_count(g_img, g_len) == 1, "one unresolved import counted");

    /* ---- import allow-list (issue #34) ---- */
    CHECK(elf64_disallowed_imports(g_img, g_len, (const char *const *)0, 0) == 1,
          "with no allow-list, the lone import is disallowed");
    const char *allow_one[] = { "ext_import" };
    CHECK(elf64_disallowed_imports(g_img, g_len, allow_one, 1) == 0,
          "allow-listing that import permits it");
    const char *allow_other[] = { "something_else" };
    CHECK(elf64_disallowed_imports(g_img, g_len, allow_other, 1) == 1,
          "allow-listing a different name still rejects the import");

    /* ---- rejection of malformed images ---- */
    CHECK(elf64_validate(g_img, 8) < 0, "truncated header rejected");
    uint8_t bad = g_img[0]; g_img[0] = 0;
    CHECK(elf64_validate(g_img, g_len) < 0, "bad magic rejected");
    g_img[0] = bad;
    uint8_t m = g_img[18]; g_img[18] = 0x3e;   /* e_machine = EM_X86_64 */
    CHECK(elf64_validate(g_img, g_len) < 0, "non-AArch64 machine rejected");
    g_img[18] = m;
    CHECK(elf64_validate(g_img, g_len) == 0, "image valid again after restore");

    printf("=== %s ===\n", g_fail ? "FAILURES PRESENT" : "ALL TESTS PASSED");
    return g_fail ? 1 : 0;
}
