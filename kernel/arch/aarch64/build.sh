#!/bin/bash
set -e
CC=aarch64-linux-gnu-gcc; LD=aarch64-linux-gnu-ld
CF="-ffreestanding -nostdlib -mgeneral-regs-only -O2"
CFI="$CF -I../../boot -I../../lib/mbedtls/include"
ARCH="boot.S vectors.S uart.c klib.c exceptions.c mmu.c heap.c gic.c timer.c smp.c stubs.c kmain.c"
OBJ=""
for f in $ARCH; do $CC $CF -w -c "$f" -o "${f%.*}.o"; OBJ="$OBJ ${f%.*}.o"; done
$CC $CFI -w -c ../../boot/std_btree.c -o std_btree.o
$CC $CFI -w -c ../../boot/zplus.c     -o zplus.o
$LD -T linker.ld $OBJ std_btree.o zplus.o -o zeos-aarch64.elf
