# xv6-riscv: scheduling and virtual memory

This branch contains two operating-systems assignments implemented on a newer
xv6-riscv snapshot: an EEVDF-inspired scheduler with supporting system calls,
and a virtual-memory extension covering lazy allocation and a small
assignment-specific `mmap` interface.

Page replacement is intentionally kept on
[`feature/page_replacement`](https://github.com/Heejoon-Kwon/xv6-riscv-os-project/tree/feature/page_replacement).
That branch uses a different xv6 snapshot, timer path, disk layout, and test
environment; the two branches are not intended to be merged.

## Implemented work

### Scheduling and process control

- Replaced the round-robin selection loop with an EEVDF-inspired policy that
  selects the eligible runnable process with the earliest virtual deadline.
- Added per-process nice values (`0` through `39`), a fixed nice-to-weight
  table, runtime and virtual-runtime accounting, virtual deadlines, and a
  five-tick base slice.
- Connected timer interrupts to runtime accounting, deadline refresh, and
  preemption at the end of a slice.
- Added `getnice`, `setnice`, `ps`, `meminfo`, and `waitpid` system calls and
  their user-space stubs.
- Extended `fork` so a child inherits its parent's nice value and starts from
  the parent's virtual-runtime position.

This is a teaching-scale EEVDF interpretation, not a port of Linux's scheduler.
The branch defaults to one QEMU CPU and makes no production/SMP-performance
claim.

### Virtual memory

- Added lazy heap growth through `sbrklazy`; first access allocates and maps a
  zero-filled page.
- Made user traps and kernel `copyin`, `copyout`, and `copyinstr` paths resolve
  eligible lazy faults.
- Added page-aligned anonymous and inode-backed mappings, optional eager
  population with `MAP_POPULATE`, read/read-write protection, and whole-mapping
  `munmap`.
- Added physical-page reference counts so resident mapped pages can be shared
  across `fork` and released when the final mapping disappears.
- Added free-page accounting through `freemem` and cleanup of empty page-table
  levels after unmapping.

The `mmap` API is deliberately smaller than POSIX: its address argument is a
page-aligned offset from `MMAPBASE`, lengths must be page-aligned, `munmap`
accepts only the mapping's start and removes the entire mapping, and there is no
`MAP_PRIVATE`/`MAP_SHARED` distinction. Resident mappings are shared across
`fork`; file-backed pages are initialized from the inode and are not written
back on unmap.

## Code map

| Area | Primary files |
| --- | --- |
| Scheduler selection, process state, nice values, `waitpid` | [`kernel/proc.c`](../kernel/proc.c), [`kernel/proc.h`](../kernel/proc.h) |
| Timer accounting and page-fault dispatch | [`kernel/trap.c`](../kernel/trap.c) |
| System-call dispatch and handlers | [`kernel/syscall.c`](../kernel/syscall.c), [`kernel/sysproc.c`](../kernel/sysproc.c), [`user/usys.pl`](../user/usys.pl) |
| Mapping table, fault resolution, mapping lifecycle | [`kernel/vm.c`](../kernel/vm.c), [`kernel/vm.h`](../kernel/vm.h) |
| Physical-page reference and free-page accounting | [`kernel/kalloc.c`](../kernel/kalloc.c) |
| Workload and regression programs | [`user/mytest.c`](../user/mytest.c), [`user/eevdf_test.c`](../user/eevdf_test.c), [`user/vm_test.c`](../user/vm_test.c) |

## Build and run

### Prerequisites

- GNU Make, a host C compiler, Perl, and `bc`
- A RISC-V 64-bit GCC/binutils toolchain recognized by the Makefile
  (`riscv64-unknown-elf-`, `riscv64-elf-`, `riscv64-none-elf-`,
  `riscv64-linux-gnu-`, or `riscv64-unknown-linux-gnu-`)
- `qemu-system-riscv64` version 6.2 or newer

From this branch's repository root:

```sh
make clean
make qemu
```

At the xv6 shell, run the focused programs individually:

```text
mytest
eevdf_test
vm_test
```

Use `Ctrl-a x` to leave QEMU.

## What the programs check

| Program | Purpose | Result style |
| --- | --- | --- |
| `mytest` | Smoke test for nice values, process reporting, memory reporting, `fork`, and `waitpid` | Prints values for inspection |
| `eevdf_test` | CPU-bound weighted-fairness workload plus sleeper/wakeup eligibility snapshots | Observational; inspect the `ps` output rather than treating it as a formal proof |
| `vm_test` | Lazy heap faults, kernel copies into/out of lazy pages, mapping validation, anonymous/file-backed mappings, eager versus lazy population, `fork`, unmap, and free-page restoration | Self-checking; prints `=== vm_test PASS ===` only when all checks succeed |

There is no CI configuration in this branch; these are manual QEMU tests.

## Attribution

This branch is coursework built on
[MIT xv6-riscv](https://github.com/mit-pdos/xv6-riscv). The original xv6
authors and contributors retain credit for the base kernel; see [`LICENSE`](../LICENSE).
The descriptions above refer to the custom assignment changes, not to the
complete xv6 codebase.
