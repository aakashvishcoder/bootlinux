// Memory-mapped devices and the system bus.
#pragma once
#include "common.h"
#include <deque>
#include <vector>

// Core-local interruptor: the machine timer (mtime/mtimecmp) and the
// machine software interrupt (msip). Only M-mode code touches it; under the
// built-in SBI, Linux reaches the timer through SBI calls instead.
struct Clint {
	u64 mtime = 0;
	u64 mtimecmp = ~0ull;
	u32 msip = 0;
	u32 read(u32 off);
	void write(u32 off, u32 val);
};

// Platform-level interrupt controller (SiFive PLIC). Two contexts:
// 0 = hart 0 M-mode, 1 = hart 0 S-mode. Sources are level-triggered.
struct Plic {
	static constexpr int NSRC = 32;
	u32 priority[NSRC] = {};
	u32 pending = 0;     // gateway has forwarded the source
	u32 in_flight = 0;   // claimed, waiting for completion
	u32 enable[2] = {};
	u32 threshold[2] = {};
	u32 lines = 0;       // current level of each source's wire

	void set_line(u32 src, bool level);
	bool irq_pending(int ctx) const;  // drives mip.MEIP / mip.SEIP
	u32 claim(int ctx);
	void complete(int ctx, u32 src);
	u32 read(u32 off);
	void write(u32 off, u32 val);
};

// NS16550A UART. Transmit goes straight to stdout; receive comes from a
// byte queue fed by the host terminal or a script.
struct Uart {
	u8 ier = 0, lcr = 0, mcr = 0, scr = 0, fcr = 0, dll = 0, dlm = 0;
	bool thre_ip = false;   // "transmitter empty" interrupt pending
	std::deque<u8> rx;
	std::string tx_log;     // recent output, scanned for the shell prompt

	u32 read(u32 off);
	void write(u32 off, u32 val);
	bool irq() const;
};

struct Bus {
	std::vector<u8> ram;
	u32 ram_size;
	Clint clint;
	Plic plic;
	Uart uart;

	explicit Bus(u32 ram_bytes) : ram(ram_bytes), ram_size(ram_bytes) {}

	bool in_ram(u64 pa, u32 len) const {
		return pa >= RAM_BASE && pa + len <= (u64)RAM_BASE + ram_size;
	}
	// Device accesses: return false for an unmapped address (access fault).
	bool read(u64 pa, u32 len, u32 &out);
	bool write(u64 pa, u32 len, u32 val);
	void update_irqs() { plic.set_line(UART_IRQ, uart.irq()); }
};
