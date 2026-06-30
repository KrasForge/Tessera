# Tessera syscall surface

Syscalls use the AArch64 convention: the syscall number is in `x8`, arguments
in `x0`-`x5`, the return value in `x0`, invoked with `svc #0`.  Return values
are non-negative on success and negative on error.  The numbers are defined in
`arch/arm64/usermode.h`.

## Process / I/O (M2, issue #13)

| num | name        | args (x0, x1, ...)            | returns                       |
|-----|-------------|-------------------------------|-------------------------------|
| 1   | `SYS_WRITE` | fd, buf, len                  | bytes written (writes to UART)|
| 2   | `SYS_EXIT`  | code                          | does not return               |
| 3   | `SYS_YIELD` | -                             | 0 (cooperative reschedule)    |

## Audio-graph control plane (M6/M7)

These are the minimal control surface the host process uses to manage plugins
and the audio graph at runtime (issues #28 and #30).

| num | name                   | args                              | returns                          |
|-----|------------------------|-----------------------------------|----------------------------------|
| 4   | `SYS_GRAPH_CONNECT`    | src_pid, dst_pid                  | 0, or negative error             |
| 5   | `SYS_GRAPH_DISCONNECT` | src_pid, dst_pid                  | 0, or negative error             |
| 6   | `SYS_GRAPH_LIST`       | -                                 | current edge count               |
| 7   | `SYS_PLUGIN_LOAD`      | path (const char\*)               | new pid (> 0), or negative error |
| 8   | `SYS_PLUGIN_UNLOAD`    | pid                               | 0, or negative error             |
| 9   | `SYS_PLUGIN_SET_PARAM` | pid, param_id, value_bits         | 0, or negative error             |

### Argument notes

- `path` for `SYS_PLUGIN_LOAD` is a pointer, in the caller's address space, to a
  NUL-terminated plugin name resolved against the in-memory plugin registry
  (the stand-in for storage / a ramdisk).  The return value is the new
  process's PID.
- `value_bits` for `SYS_PLUGIN_SET_PARAM` is the 32-bit IEEE-754 bit pattern of
  the float parameter value (the control path carries no floating point in the
  kernel).  The plugin reinterprets the bits as a `float`.
- The DAC sink is addressed as PID 0 in `SYS_GRAPH_CONNECT` / `_DISCONNECT`.

### Return / error codes

| value | meaning                                   |
|-------|-------------------------------------------|
| `>= 0`| success (`SYS_PLUGIN_LOAD` returns a pid; `SYS_GRAPH_LIST` an edge count) |
| -1    | not found (unknown plugin name, pid, or edge) / unimplemented |
| -2    | out of resources, or the edge already exists (duplicate connect)        |
| -3    | bad ELF / no such edge to disconnect      |
| -4    | parameter queue full                      |

(The exact negative codes are the `PM_E*` / `GC_E*` constants in
`plugin_mgr.h` and `graph_control.h`.)

### Semantics

- **Load** creates a fresh MMU-isolated process (issue #24), maps a per-plugin
  lock-free parameter queue into it, and registers it as a graph node.
- **Unload** disconnects every edge touching the plugin, destroys the process,
  and frees every page it owns - segments, stack, trampoline, parameter page,
  and the parameter queue - along with its page tables and ASID.  Repeated
  load/unload returns the physical allocator to its baseline (no leak).
- **Set-param** pushes `(param_id, value_bits)` onto the plugin's lock-free
  parameter queue; the plugin drains it at the top of its next
  `process_block`, so the value is delivered within one audio block.
- **Connect / disconnect** allocate or free a shared ring-buffer edge and update
  the graph under a seqlock generation, so a rewire never blocks the audio
  thread (issue #28).

All of these are callable from a user-space host process via `svc #0`.
