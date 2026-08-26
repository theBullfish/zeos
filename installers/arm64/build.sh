#!/bin/bash
set -e
CC=aarch64-linux-gnu-gcc; LD=aarch64-linux-gnu-ld
CF="-ffreestanding -nostdlib -mgeneral-regs-only -O2 -w -Ishim -I../../os/boot"
FP="-ffreestanding -nostdlib -O2 -w -Ishim -I../../os/boot -I../../os/lib/mbedtls/include"
for f in boot.S vectors.S fdt.c platform.c uart.c klib.c exceptions.c mmu.c heap.c gic.c timer.c smp.c ramfb.c hal_arm64.c stubs.c kmain.c; do $CC $CF -c "$f" -o "${f%.*}.o"; done
for m in std_btree zplus signal zp_runtime fb; do $CC $FP -c ../../os/boot/$m.c -o $m.o; done
$LD -T linker.ld boot.o vectors.o fdt.o platform.o uart.o klib.o exceptions.o mmu.o heap.o gic.o timer.o smp.o ramfb.o hal_arm64.o stubs.o kmain.o std_btree.o zplus.o signal.o zp_runtime.o fb.o -o zeos-aarch64.elf
