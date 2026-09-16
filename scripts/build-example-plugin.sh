#!/bin/sh
# scripts/build-example-plugin.sh - build and verify the SDK example plugin
# exactly as the getting-started guide (docs/getting-started.md) instructs.
# This is the single source of truth the guide quotes and CI runs, so the two
# cannot drift.
#
#   CROSS_COMPILE=aarch64-none-elf-  ./scripts/build-example-plugin.sh
#
# Override CROSS_COMPILE for your toolchain triple (CI uses aarch64-linux-gnu-).
set -eu

CROSS_COMPILE="${CROSS_COMPILE:-aarch64-none-elf-}"
READELF="${CROSS_COMPILE}readelf"
ELF=sdk/examples/sine_plugin/sine_plugin.elf

echo "== Building the example plugin with the SDK (CROSS_COMPILE=$CROSS_COMPILE) =="
make -C sdk/examples/sine_plugin CROSS_COMPILE="$CROSS_COMPILE"

echo "== Verifying $ELF =="

# 1. A little-endian AArch64 ELF.
"$READELF" -h "$ELF" | grep -q 'AArch64' \
  || { echo "FAIL: not an AArch64 ELF"; exit 1; }
echo "  ok: AArch64 ELF"

# 2. Exports all five required ABI symbols.
for s in plugin_abi_version plugin_init plugin_process_block \
         plugin_set_param plugin_destroy; do
    "$READELF" -sW "$ELF" | grep -q " $s\$" \
      || { echo "FAIL: missing ABI symbol $s"; exit 1; }
done
echo "  ok: all five ABI symbols present"

# 3. Self-contained: no undefined (imported) named symbols.
u=$("$READELF" -sW "$ELF" | awk '$7=="UND" && $8!=""' | wc -l)
[ "$u" -eq 0 ] || { echo "FAIL: $u disallowed import(s)"; exit 1; }
echo "  ok: 0 disallowed imports"

echo "== sine_plugin.elf is a valid, self-contained Tessera plugin =="
