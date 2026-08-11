# Phase 0: Stop Crashing — Implementation Status

> Last updated: 2026-03-27
> Context: Audit identified 8 critical items. This tracks progress.

## Completed

### 1. Reserve kernel image in PMM
- **File**: `os/boot/pmm.c`
- **What**: Added `pmm_reserve_range()` function. At end of `pmm_init()`, reserves:
  - Kernel `.text` through `.edata` (via linker symbols `_text`, `_edata`)
  - Falls back to 4MB at 1MB if linker symbols aren't usable
  - UEFI memory map buffer itself
- **Risk**: The `extern char _text, _edata` symbols depend on the GNU-EFI linker
  script (`elf_x86_64_efi.lds`) defining them. **Verify on Zorin build** that these
  symbols exist. If not, adjust to whatever the linker script provides, or add
  `PROVIDE(_text = .);` etc. to a custom linker section.
- **Test**: Boot in QEMU, run `mem` in shell — free count should be slightly lower
  than before (kernel + mmap buffer now reserved).

### 2. Create GDT + TSS
- **Files**: `installers/x86_64/gdt.c`, `installers/x86_64/gdt.h` (NEW)
- **What**:
  - 5-entry GDT: null, kernel code64, kernel data, user code64, user data, + TSS
  - TSS with 3 IST stacks (double fault, NMI, machine check) — 16KB each from PMM
  - Loads GDT via LGDT, reloads all segment registers, loads TSS via LTR
- **Init order**: Called AFTER pmm_init/vmm_init/heap_init, BEFORE idt_init
- **Test**: If GDT is wrong, first interrupt after `sti` will triple-fault. If boot
  reaches the shell prompt, GDT is correct.

### 3. Exception handlers (vectors 0-20)
- **File**: `installers/x86_64/idt.c`
- **What**:
  - Added `ISR_STUB_ERR` macro for exceptions that push hardware error codes (8, 10-14, 17)
  - Added `ISR_STUB_NOERR` stubs for all other exceptions (0-7, 9, 15-16, 18-20)
  - All 21 exception stubs installed in `idt_init()` with proper GDT_KERNEL_CODE selector
  - IST assignments: vector 8 (DF) → IST1, vector 2 (NMI) → IST2, vector 18 (MC) → IST3
  - Unhandled exceptions in `isr_dispatch` now call `panic_with_state()` with register dump
- **Test**: In shell, trigger a divide-by-zero or invalid opcode. Should see panic
  screen with register dump instead of silent reboot.

### 4. Panic with register dump
- **Files**: `os/boot/panic.c`, `os/boot/panic.h` (NEW)
- **What**:
  - `panic(msg)` — prints message, halts with infinite `hlt` loop
  - `panic_with_state(msg, vector, error_code, regs)` — prints exception name,
    error code decode (page fault address/cause, GPF selector index),
    full register dump (RAX-R15, RIP, RFLAGS, CR0-CR4, CS, SS)
  - Human-readable exception names for vectors 0-21
- **Test**: Any exception should produce useful diagnostic output.

### 5. Disable UEFI watchdog
- **File**: `os/boot/main.c`
- **What**: Added `SetWatchdogTimer(0, 0, 0, NULL)` call before ExitBootServices
- **Why**: UEFI firmware sets a 5-minute watchdog. If kernel stalls during
  calibration or PCI scan, the watchdog resets the machine silently.

### 6. Fix heap coalesce adjacency check
- **File**: `os/boot/heap.c`
- **What**: Added `blocks_adjacent()` check before merging in `coalesce()`.
  Only merges blocks where `block + header + size == next_block`.
  Prevents corruption when heap expands to non-contiguous regions.
- **Also**: Removed dead `heap_expand()` function (allocated pages but never
  linked them into the free list).

### 7. Fix mkdir to create directories
- **Files**: `os/boot/vault.c`, `os/boot/vault.h`, `os/boot/shell.c`
- **What**: Added `vault_mkdir()` function that sets `VAULT_TYPE_DIR` instead of
  `VAULT_TYPE_FILE`. Shell's `cmd_mkdir` now calls `vault_mkdir()`.
- **Before**: `mkdir /home/brad` created a file inode. Children couldn't be created.
- **After**: Creates a proper directory inode. Path resolution works for children.

### 8. Panic safety net after shell
- **File**: `os/boot/main.c`
- **What**: Added `panic("shell_run returned unexpectedly")` after `shell_run()`.
  Previously, if shell returned, execution fell through to `return EFI_SUCCESS`
  which jumped to the UEFI CRT0 stub — now in freed/overwritten memory.
- **Also**: Added `panic()` calls if PMM or heap init fails.

## Updated build
- **Makefile**: Added `boot/panic.c` and `boot/gdt.c` to SRCS.
  Split SRCS across multiple lines for readability.

## NOT done yet (remaining Phase 0)
- **Dedicated kernel stack**: Not allocated. Still running on UEFI's stack.
  Adding this requires knowing the stack's current location (or just switching
  RSP after allocating from PMM). Deferred to avoid introducing a stack-switch
  bug without testing on real hardware.

## To verify on Zorin
1. `make clean && make all` — does it compile?
2. `make run` — does it boot to shell prompt?
3. In shell, try an invalid command or memory stress test — do exceptions produce
   diagnostic output instead of silent reboot?
4. Check that `_text` and `_edata` linker symbols resolve. If not, adjust `pmm.c`
   to use the symbols the linker script actually provides (check with `nm` or `readelf`).
5. Run `mem` command — verify free memory is slightly lower (kernel reserved).
6. Run `mkdir /test && ls /` — verify /test shows as a directory.

## What's next: Phase 1 (Real Hardware)
See `docs/AUDIT_2026_03_27.md` Part 7 for the full roadmap.
Priority: APIC, PCI capabilities, NVMe, IOMMU.
