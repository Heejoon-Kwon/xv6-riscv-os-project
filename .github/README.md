# xv6-riscv: page replacement and swapping

This branch implements demand-driven page replacement for xv6-riscv. When the
physical-page free list is exhausted, the allocator selects a user page with a
global second-chance/clock policy, writes it to a reserved area of `fs.img`, and
reuses the physical frame. A later access faults the page back into memory.

The scheduler and separate virtual-memory assignment are kept on
[`develop`](https://github.com/Heejoon-Kwon/xv6-riscv-os-project/tree/develop).
That branch uses a different xv6 snapshot, timer path, disk layout, and test
environment; the two branches are not intended to be merged.

## Implemented work

### Replacement path

- Added metadata for physical frames and a circular, doubly linked replacement
  list of mapped user pages.
- Integrated eviction with `kalloc`: allocation falls back to `swapout` only
  after the normal free list is empty.
- Implemented a second-chance scan using the RISC-V PTE accessed bit (`PTE_A`).
  Referenced pages have the bit cleared and move past the clock hand; the first
  eligible unreferenced page becomes the victim.
- Added a bitmap allocator for swap slots and encoded the slot number in an
  invalid user PTE while the page is non-resident.
- Added synchronous page-sized swap reads/writes through xv6's buffer cache,
  using a temporary per-process mapping for the physical frame.

### Faults and lifecycle

- Restored swapped pages from user instruction, load, and store page faults.
- Made `walkaddr` restore a swapped page when kernel code accesses a user
  buffer through the copy helpers.
- Released swap slots when non-resident pages are unmapped.
- Handled `fork` by bringing non-resident parent pages back before xv6 copies
  the address space; covered process exit/deallocation and `exec` behavior in
  the stress program.
- Added swap I/O counters and test-facing system calls used to confirm that a
  workload caused real disk traffic rather than merely fitting in RAM.

## Design flow

```text
kalloc finds no free frame
          |
          v
scan global user-page ring -- PTE_A set --> clear bit, advance hand
          |
          v
allocate swap slot -> encode slot in invalid PTE -> write 4 KiB -> reuse frame
          |
          v
later page fault -> allocate/reclaim frame -> read slot -> restore valid PTE
```

The disk image contains 30,000 blocks of 1 KiB each. Normal filesystem data is
kept below block 2,000; blocks 2,000 through 29,999 are reserved for swapping.
With four blocks per 4 KiB page, this provides 7,000 swap slots.

## Code map

| Area | Primary files |
| --- | --- |
| Frame metadata, clock scan, swap-slot bitmap, swap-in/out | [`kernel/kalloc.c`](../kernel/kalloc.c) |
| Swapped-PTE handling during address translation, mapping, unmapping, and `fork` | [`kernel/vm.c`](../kernel/vm.c), [`kernel/riscv.h`](../kernel/riscv.h) |
| User page-fault dispatch | [`kernel/trap.c`](../kernel/trap.c) |
| Swap-region layout and block I/O | [`kernel/param.h`](../kernel/param.h), [`kernel/fs.c`](../kernel/fs.c), [`mkfs/mkfs.c`](../mkfs/mkfs.c) |
| Test instrumentation system calls | [`kernel/sysfile.c`](../kernel/sysfile.c), [`kernel/syscall.c`](../kernel/syscall.c) |
| Memory-pressure regression program | [`user/swaptest.c`](../user/swaptest.c) |

## Build and run

This branch must be built independently from `develop`.

### Prerequisites

- GNU Make, a host C compiler, and Perl
- A RISC-V 64-bit GCC/binutils toolchain recognized by the Makefile
  (`riscv64-unknown-elf-`, `riscv64-linux-gnu-`, or
  `riscv64-unknown-linux-gnu-`)
- `qemu-system-riscv64` version 7.2 or newer; this xv6 snapshot uses the RISC-V
  `sstc` extension and `stimecmp` for supervisor-mode timer interrupts

Force a clean rebuild with reduced emulated RAM so the test necessarily
reaches the eviction path:

```sh
make clean
make -B PHYSTOP_MB=32 CPUS=1 qemu
```

At the xv6 shell:

```text
swaptest
```

Expected final line:

```text
swaptest: all tests passed
```

Use `Ctrl-a x` to leave QEMU. Running the test with the normal 128 MiB setting
can make it fail its swap-activity checks because the workload may fit in RAM;
that is why the command above uses `PHYSTOP_MB=32`. `-B` is important when
changing that Make variable because it forces objects to be rebuilt with the
new physical-memory limit.

## Test coverage

`swaptest` is a six-part, self-checking workload:

1. Basic swap-out, swap-in, and byte-pattern preservation.
2. Accessed-bit/clock stress with repeatedly touched pages.
3. `fork` while the parent owns pages under swap pressure, including isolation
   of the child's later writes.
4. Repeated allocation and deallocation to exercise swap-slot cleanup.
5. Concurrent memory pressure from four child processes plus the parent.
6. `exec("ls")` after the replacement workload.

Each memory-pressure phase checks that both swap-read and swap-write counters
increase. There is no CI configuration in this branch; the QEMU test is manual.

## Limits

This is a teaching implementation, not a production virtual-memory subsystem.
It uses one global replacement ring and synchronous I/O, and does not implement
copy-on-write, asynchronous writeback, clustered I/O, per-process replacement,
or a general swap device. The documented test configuration is single-core;
no SMP scalability claim is made. The raw swap system calls are assignment test
interfaces, not application-facing APIs.

## Attribution

This branch is coursework built on
[MIT xv6-riscv](https://github.com/mit-pdos/xv6-riscv). The original xv6
authors and contributors retain credit for the base kernel; see [`LICENSE`](../LICENSE).
The descriptions above refer to the custom assignment changes, not to the
complete xv6 codebase.
