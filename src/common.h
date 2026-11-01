// Shared types and constants for the bootlinux RV32 emulator.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;
using i64 = int64_t;

// Privilege levels (the encoding used in mstatus.MPP and friends).
enum Priv : u32 { PRIV_U = 0, PRIV_S = 1, PRIV_M = 3 };

// Exception causes (mcause/scause with the interrupt bit clear).
enum Exc : u32 {
	EXC_INSN_MISALIGNED = 0,
	EXC_INSN_ACCESS = 1,
	EXC_ILLEGAL_INSN = 2,
	EXC_BREAKPOINT = 3,
	EXC_LOAD_MISALIGNED = 4,
	EXC_LOAD_ACCESS = 5,
	EXC_STORE_MISALIGNED = 6,
	EXC_STORE_ACCESS = 7,
	EXC_ECALL_U = 8,
	EXC_ECALL_S = 9,
	EXC_ECALL_M = 11,
	EXC_INSN_PAGE_FAULT = 12,
	EXC_LOAD_PAGE_FAULT = 13,
	EXC_STORE_PAGE_FAULT = 15,
};

// Interrupt numbers (bit positions in mip/mie).
enum Irq : u32 {
	IRQ_SSI = 1, IRQ_MSI = 3,
	IRQ_STI = 5, IRQ_MTI = 7,
	IRQ_SEI = 9, IRQ_MEI = 11,
};

// A synchronous exception, thrown from deep inside the memory system or
// decoder and caught once per instruction in Cpu::step().
struct Trap {
	u32 cause;
	u32 tval;
};

// Kind of memory access, used by the MMU for permission checks and to pick
// which fault to raise.
enum Access { ACC_FETCH, ACC_LOAD, ACC_STORE };

[[noreturn]] inline void fatal(const std::string &msg)
{
	fprintf(stderr, "\nbootlinux: fatal: %s\n", msg.c_str());
	exit(1);
}

// Memory map (same layout as QEMU's "virt" machine).
constexpr u32 CLINT_BASE = 0x02000000, CLINT_SIZE = 0x10000;
constexpr u32 PLIC_BASE = 0x0c000000, PLIC_SIZE = 0x600000;
constexpr u32 UART_BASE = 0x10000000, UART_SIZE = 0x100;
constexpr u32 RAM_BASE = 0x80000000;
constexpr u32 UART_IRQ = 10;

// Timer: mtime ticks at 10 MHz and is derived from the instruction count
// (one instruction = 10 ns, i.e. a nominal 100 MIPS machine). Tying time to
// instructions makes every boot deterministic, which the TLB study needs.
constexpr u64 TIMEBASE_HZ = 10'000'000;
constexpr u64 INSNS_PER_TICK = 10;
