#include "devices.h"
#include <cstring>

// ---------------------------------------------------------------- CLINT
u32 Clint::read(u32 off)
{
	switch (off) {
	case 0x0000: return msip;
	case 0x4000: return (u32)mtimecmp;
	case 0x4004: return (u32)(mtimecmp >> 32);
	case 0xbff8: return (u32)mtime;
	case 0xbffc: return (u32)(mtime >> 32);
	}
	return 0;
}

void Clint::write(u32 off, u32 val)
{
	switch (off) {
	case 0x0000: msip = val & 1; break;
	case 0x4000: mtimecmp = (mtimecmp & ~0xffffffffull) | val; break;
	case 0x4004: mtimecmp = (mtimecmp & 0xffffffffull) | ((u64)val << 32); break;
	case 0xbff8: mtime = (mtime & ~0xffffffffull) | val; break;
	case 0xbffc: mtime = (mtime & 0xffffffffull) | ((u64)val << 32); break;
	}
}

// ----------------------------------------------------------------- PLIC
void Plic::set_line(u32 src, bool level)
{
	u32 bit = 1u << src;
	if (level) lines |= bit; else lines &= ~bit;
	// Gateway: a high line becomes pending unless already claimed.
	if (level && !(in_flight & bit))
		pending |= bit;
	if (!level)
		pending &= ~bit;
}

static int best_source(const Plic &p, int ctx)
{
	int best = 0;
	u32 best_prio = p.threshold[ctx];
	u32 cand = p.pending & p.enable[ctx] & ~p.in_flight;
	for (int s = 1; s < Plic::NSRC; s++)
		if ((cand >> s) & 1 && p.priority[s] > best_prio) {
			best = s;
			best_prio = p.priority[s];
		}
	return best;
}

bool Plic::irq_pending(int ctx) const { return best_source(*this, ctx) != 0; }

u32 Plic::claim(int ctx)
{
	int s = best_source(*this, ctx);
	if (s) {
		pending &= ~(1u << s);
		in_flight |= 1u << s;
	}
	return s;
}

void Plic::complete(int, u32 src)
{
	if (src >= NSRC) return;
	in_flight &= ~(1u << src);
	if ((lines >> src) & 1)
		pending |= 1u << src;   // still asserted: re-arm
}

u32 Plic::read(u32 off)
{
	if (off < 0x1000) return priority[(off >> 2) % NSRC];
	if (off == 0x1000) return pending;
	if (off >= 0x2000 && off < 0x2000 + 0x80 * 2)
		return (off & 0x7f) == 0 ? enable[(off - 0x2000) / 0x80] : 0;
	if (off >= 0x200000) {
		int ctx = (off - 0x200000) / 0x1000;
		if (ctx > 1) return 0;
		if ((off & 0xfff) == 0) return threshold[ctx];
		if ((off & 0xfff) == 4) return claim(ctx);
	}
	return 0;
}

void Plic::write(u32 off, u32 val)
{
	if (off < 0x1000) { priority[(off >> 2) % NSRC] = val & 7; return; }
	if (off >= 0x2000 && off < 0x2000 + 0x80 * 2) {
		if ((off & 0x7f) == 0) enable[(off - 0x2000) / 0x80] = val & ~1u;
		return;
	}
	if (off >= 0x200000) {
		int ctx = (off - 0x200000) / 0x1000;
		if (ctx > 1) return;
		if ((off & 0xfff) == 0) threshold[ctx] = val & 7;
		if ((off & 0xfff) == 4) complete(ctx, val);
	}
}

// ----------------------------------------------------------------- UART
// Register offsets (byte-wide, stride 1).
enum { RBR_THR = 0, IER = 1, IIR_FCR = 2, LCR = 3, MCR = 4, LSR = 5, MSR = 6, SCR = 7 };

u32 Uart::read(u32 off)
{
	bool dlab = lcr & 0x80;
	switch (off) {
	case RBR_THR:
		if (dlab) return dll;
		if (rx.empty()) return 0;
		{ u8 c = rx.front(); rx.pop_front(); return c; }
	case IER: return dlab ? dlm : ier;
	case IIR_FCR: {
		u32 fifo = (fcr & 1) ? 0xc0 : 0;
		if ((ier & 1) && !rx.empty()) return fifo | 0x04;   // RX data
		if ((ier & 2) && thre_ip) { thre_ip = false; return fifo | 0x02; }
		return fifo | 0x01;                                // none
	}
	case LCR: return lcr;
	case MCR: return mcr;
	case LSR: return 0x60 | (rx.empty() ? 0 : 1);           // TX always empty
	case MSR: return 0xb0;                                  // DCD, DSR, CTS
	case SCR: return scr;
	}
	return 0;
}

void Uart::write(u32 off, u32 val)
{
	bool dlab = lcr & 0x80;
	switch (off) {
	case RBR_THR:
		if (dlab) { dll = val; break; }
		putchar((int)val);
		fflush(stdout);
		tx_log.push_back((char)val);
		if (tx_log.size() > 64) tx_log.erase(0, tx_log.size() - 64);
		thre_ip = true;
		break;
	case IER:
		if (dlab) { dlm = val; break; }
		// Enabling the TX-empty interrupt fires it immediately (THR is empty).
		if ((val & 2) && !(ier & 2)) thre_ip = true;
		ier = val & 0x0f;
		break;
	case IIR_FCR: fcr = val; if (val & 2) rx.clear(); break;
	case LCR: lcr = val; break;
	case MCR: mcr = val; break;
	case SCR: scr = val; break;
	}
}

bool Uart::irq() const
{
	return ((ier & 1) && !rx.empty()) || ((ier & 2) && thre_ip);
}

// ------------------------------------------------------------------ Bus
bool Bus::read(u64 pa, u32 len, u32 &out)
{
	if (in_ram(pa, len)) {
		out = 0;
		memcpy(&out, &ram[pa - RAM_BASE], len);
		return true;
	}
	if (pa >= CLINT_BASE && pa < CLINT_BASE + CLINT_SIZE) {
		out = clint.read((u32)pa - CLINT_BASE);
		return true;
	}
	if (pa >= PLIC_BASE && pa < PLIC_BASE + PLIC_SIZE) {
		out = plic.read((u32)pa - PLIC_BASE);
		return true;
	}
	if (pa >= UART_BASE && pa < UART_BASE + UART_SIZE) {
		out = uart.read((u32)pa - UART_BASE) & 0xff;
		update_irqs();
		return true;
	}
	return false;
}

bool Bus::write(u64 pa, u32 len, u32 val)
{
	if (in_ram(pa, len)) {
		memcpy(&ram[pa - RAM_BASE], &val, len);
		return true;
	}
	if (pa >= CLINT_BASE && pa < CLINT_BASE + CLINT_SIZE) {
		clint.write((u32)pa - CLINT_BASE, val);
		return true;
	}
	if (pa >= PLIC_BASE && pa < PLIC_BASE + PLIC_SIZE) {
		plic.write((u32)pa - PLIC_BASE, val);
		return true;
	}
	if (pa >= UART_BASE && pa < UART_BASE + UART_SIZE) {
		uart.write((u32)pa - UART_BASE, val & 0xff);
		update_irqs();
		return true;
	}
	return false;
}
