#include "cpu.h"
#include <cstring>

Cpu::Cpu(Bus &bus, const CpuConfig &cfg)
	: itlb(cfg.itlb_entries), dtlb(cfg.dtlb_entries), bus_(bus), cfg_(cfg) {}

void Cpu::reset(u32 start_pc, Priv p)
{
	memset(x, 0, sizeof(x));
	pc = start_pc;
	priv = p;
	invalidate_fetch();
}

// ===================================================================
// Main loop
// ===================================================================

void Cpu::run(u64 max_insns)
{
	u64 end = instret + max_insns;
	while (stop == StopReason::None && instret < end)
		step();
}

void Cpu::step()
{
	// Time: mtime advances one tick every INSNS_PER_TICK instructions.
	if (++tick_count_ >= (int)INSNS_PER_TICK) {
		tick_count_ = 0;
		bus_.clint.mtime++;
	}

	if (check_interrupts())
		return;

	u32 insn_pc = pc;
	try {
		u32 insn = fetch();
		if (trace_left) {
			trace_left--;
			fprintf(stderr, "[%c] %08x: %08x\n", "US?M"[priv], insn_pc, insn);
		}
		Priv p = priv;
		execute(insn);
		stats.insns[p]++;
		instret++;
	} catch (const Trap &t) {
		pc = insn_pc;
		take_trap(t.cause, t.tval, false);
	}
	x[0] = 0;
}

// ===================================================================
// Interrupts and traps
// ===================================================================

void Cpu::refresh_ext()
{
	bus_.update_irqs();
	ext_m_ = bus_.plic.irq_pending(0);
	ext_s_ = bus_.plic.irq_pending(1);
}

// mip as software sees it: software-written bits plus device lines.
u32 Cpu::pending_irqs() const
{
	u32 ip = mip_sw_;
	if (bus_.clint.mtime >= bus_.clint.mtimecmp) ip |= 1u << IRQ_MTI;
	if (bus_.clint.msip) ip |= 1u << IRQ_MSI;
	if (ext_m_) ip |= 1u << IRQ_MEI;
	if (ext_s_) ip |= 1u << IRQ_SEI;
	// Built-in SBI: the "firmware" raises STIP when the SBI timer expires,
	// which is what OpenSBI does from its M-mode timer handler.
	if (cfg_.builtin_sbi && bus_.clint.mtime >= sbi_timecmp) ip |= 1u << IRQ_STI;
	return ip;
}

bool Cpu::check_interrupts()
{
	u32 pend = pending_irqs() & mie_;
	if (!pend) return false;

	bool m_enabled = priv < PRIV_M || (mstatus & MS_MIE);
	bool s_enabled = priv < PRIV_S || (priv == PRIV_S && (mstatus & MS_SIE));
	u32 m_pend = m_enabled ? pend & ~mideleg : 0;
	u32 s_pend = s_enabled ? pend & mideleg : 0;
	u32 take = m_pend ? m_pend : s_pend;
	if (!take) return false;

	// Standard priority order.
	static const u32 order[] = {IRQ_MEI, IRQ_MSI, IRQ_MTI, IRQ_SEI, IRQ_SSI, IRQ_STI};
	for (u32 irq : order)
		if (take & (1u << irq)) {
			take_trap(irq, 0, true);
			return true;
		}
	return false;
}

void Cpu::take_trap(u32 cause, u32 tval, bool interrupt)
{
	if (interrupt) stats.interrupts[cause & 15]++;
	else stats.exceptions[cause & 15]++;

	u32 deleg = interrupt ? mideleg : medeleg;
	bool to_s = priv <= PRIV_S && ((deleg >> cause) & 1);
	u32 cause_val = cause | (interrupt ? 0x80000000u : 0);

	resv_valid_ = false;
	invalidate_fetch();

	if (to_s) {
		scause_ = cause_val;
		sepc_ = pc;
		stval_ = tval;
		mstatus = (mstatus & ~MS_SPIE) | ((mstatus & MS_SIE) ? MS_SPIE : 0);
		mstatus &= ~MS_SIE;
		mstatus = (mstatus & ~MS_SPP) | (priv == PRIV_S ? MS_SPP : 0);
		priv = PRIV_S;
		pc = (stvec_ & ~3u) + ((stvec_ & 1) && interrupt ? 4 * cause : 0);
	} else {
		if (cfg_.builtin_sbi) {
			// With built-in SBI there is no M-mode firmware to handle this.
			fprintf(stderr, "\nbootlinux: unhandled trap to M-mode: cause=%s%u tval=%08x\n",
				interrupt ? "irq " : "", cause, tval);
			dump_state(stderr);
			stop = StopReason::Fatal;
			return;
		}
		mcause_ = cause_val;
		mepc_ = pc;
		mtval_ = tval;
		mstatus = (mstatus & ~MS_MPIE) | ((mstatus & MS_MIE) ? MS_MPIE : 0);
		mstatus &= ~MS_MIE;
		mstatus = (mstatus & ~MS_MPP) | ((u32)priv << 11);
		priv = PRIV_M;
		pc = (mtvec_ & ~3u) + ((mtvec_ & 1) && interrupt ? 4 * cause : 0);
	}
}

// ===================================================================
// Memory: virtual -> physical
// ===================================================================

[[noreturn]] static void page_fault(Access acc, u32 va)
{
	static const u32 cause[] = {EXC_INSN_PAGE_FAULT, EXC_LOAD_PAGE_FAULT, EXC_STORE_PAGE_FAULT};
	throw Trap{cause[acc], va};
}

[[noreturn]] static void access_fault(Access acc, u32 va)
{
	static const u32 cause[] = {EXC_INSN_ACCESS, EXC_LOAD_ACCESS, EXC_STORE_ACCESS};
	throw Trap{cause[acc], va};
}

// Can an access of kind `acc` at effective privilege `eff` use this PTE?
static bool pte_allows(u32 pte, Access acc, Priv eff, u32 mstatus)
{
	switch (acc) {
	case ACC_FETCH: if (!(pte & PTE_X)) return false; break;
	case ACC_LOAD:
		if (!(pte & PTE_R) && !((mstatus & MS_MXR) && (pte & PTE_X))) return false;
		break;
	case ACC_STORE: if (!(pte & PTE_W)) return false; break;
	}
	if (eff == PRIV_U)
		return pte & PTE_U;
	// Supervisor: never executes user pages; touches user data only with SUM.
	if (pte & PTE_U)
		return acc != ACC_FETCH && (mstatus & MS_SUM);
	return true;
}

u64 Cpu::translate(u32 va, Access acc)
{
	Priv eff = priv;
	if (acc != ACC_FETCH && (mstatus & MS_MPRV))
		eff = (Priv)((mstatus & MS_MPP) >> 11);
	if (eff == PRIV_M || !(satp_ >> 31))
		return va;

	Tlb &tlb = acc == ACC_FETCH ? itlb : dtlb;
	u32 vpn = va >> 12;
	TlbEntry *t = tlb.lookup(vpn, asid());
	if (!t) {
		t = walk(va, acc, eff, tlb);
	} else {
		if (!pte_allows(t->pte, acc, eff, mstatus))
			page_fault(acc, va);
		// First store to a clean page: re-walk so the walker sets D in memory.
		if (acc == ACC_STORE && !(t->pte & PTE_D)) {
			t->valid = false;
			t = walk(va, acc, eff, tlb);
		}
	}
	if (t->super)
		return ((u64)(t->ppn >> 10) << 22) | (va & 0x3fffff);
	return ((u64)t->ppn << 12) | (va & 0xfff);
}

// Sv32 two-level page-table walk. Fills the TLB on success.
TlbEntry *Cpu::walk(u32 va, Access acc, Priv eff, Tlb &tlb)
{
	stats.walks[acc]++;
	u64 table = (u64)(satp_ & 0x3fffff) << 12;
	u32 vpn[2] = {(va >> 12) & 0x3ff, va >> 22};
	u32 pte = 0;
	u64 pte_addr = 0;
	int level;

	for (level = 1;; level--) {
		pte_addr = table + vpn[level] * 4;
		if (!bus_.in_ram(pte_addr, 4))
			access_fault(acc, va);
		memcpy(&pte, &bus_.ram[pte_addr - RAM_BASE], 4);
		stats.pte_reads++;

		if (!(pte & PTE_V) || (!(pte & PTE_R) && (pte & PTE_W)))
			page_fault(acc, va);
		if (pte & (PTE_R | PTE_X))
			break;                          // leaf
		if (level == 0)
			page_fault(acc, va);            // pointer at the last level
		table = (u64)(pte >> 10) << 12;
	}

	u32 ppn = pte >> 10;
	if (level == 1 && (ppn & 0x3ff))
		page_fault(acc, va);                // misaligned megapage
	if (!pte_allows(pte, acc, eff, mstatus))
		page_fault(acc, va);

	// Hardware-managed Accessed/Dirty bits.
	u32 want = PTE_A | (acc == ACC_STORE ? PTE_D : 0);
	if ((pte & want) != want) {
		pte |= want;
		memcpy(&bus_.ram[pte_addr - RAM_BASE], &pte, 4);
		stats.ad_updates++;
	}

	TlbEntry e;
	e.super = level == 1;
	e.global = pte & PTE_G;
	e.vpn = va >> 12;
	e.asid = asid();
	e.ppn = ppn;
	if (e.super && cfg_.split_superpages) {
		// Model a TLB without superpage support: cache only the 4 KiB
		// piece of the megapage that this access touched.
		e.super = false;
		e.ppn = (ppn & ~0x3ffu) | vpn[0];
		stats.split_fills++;
	}
	e.pte = pte & 0xff;
	e.pte_addr = (u32)pte_addr;
	return tlb.insert(e);
}

// ===================================================================
// Memory: physical accesses
// ===================================================================

u32 Cpu::phys_read(u64 pa, int len, Access acc, u32 va)
{
	if (bus_.in_ram(pa, len)) {
		u32 v = 0;
		memcpy(&v, &bus_.ram[pa - RAM_BASE], len);
		return v;
	}
	u32 v;
	if (!bus_.read(pa, len, v))
		access_fault(acc, va);
	refresh_ext();
	return v;
}

void Cpu::phys_write(u64 pa, int len, u32 val, u32 va)
{
	if (bus_.in_ram(pa, len)) {
		memcpy(&bus_.ram[pa - RAM_BASE], &val, len);
		if (pa == tohost_addr && tohost_addr && val) {
			exit_code = val >> 1;
			stop = StopReason::ToHost;
		}
		return;
	}
	if (!bus_.write(pa, len, val))
		access_fault(ACC_STORE, va);
	refresh_ext();
}

u32 Cpu::load(u32 va, int len)
{
	if (va & (len - 1)) {
		// Misaligned: assemble from byte accesses (allowed by the spec).
		u32 v = 0;
		for (int i = 0; i < len; i++)
			v |= phys_read(translate(va + i, ACC_LOAD), 1, ACC_LOAD, va + i) << (8 * i);
		return v;
	}
	return phys_read(translate(va, ACC_LOAD), len, ACC_LOAD, va);
}

void Cpu::store(u32 va, int len, u32 val)
{
	if (va & (len - 1)) {
		for (int i = 0; i < len; i++)
			phys_write(translate(va + i, ACC_STORE), 1, (val >> (8 * i)) & 0xff, va + i);
		return;
	}
	phys_write(translate(va, ACC_STORE), len, val, va);
}

// Instruction fetch with a one-page cache. The cached page is the one the
// last fetch's ITLB lookup returned; it stays valid until the ITLB changes
// (epoch), the privilege level changes, or satp is written. A fetch that
// hits it would have hit the same ITLB entry, which is already the most
// recently used one, so we count the hit without redoing the lookup.
u32 Cpu::fetch()
{
	if (pc & 3) throw Trap{EXC_INSN_MISALIGNED, pc};
	u32 vpage = pc & ~0xfffu;
	if (vpage == fetch_vpage_ && (!fetch_tlb_ || fetch_epoch_ == itlb.epoch)) {
		if (fetch_tlb_) itlb.stats.hits++;
		u32 insn;
		memcpy(&insn, fetch_host_ + (pc & 0xfff), 4);
		return insn;
	}
	bool translated = priv != PRIV_M && (satp_ >> 31);
	u64 pa = translate(pc, ACC_FETCH);
	if (!bus_.in_ram(pa, 4))
		throw Trap{EXC_INSN_ACCESS, pc};
	fetch_vpage_ = vpage;
	fetch_host_ = &bus_.ram[(pa & ~0xfffull) - RAM_BASE];
	fetch_tlb_ = translated;
	fetch_epoch_ = itlb.epoch;
	u32 insn;
	memcpy(&insn, &bus_.ram[pa - RAM_BASE], 4);
	return insn;
}

// ===================================================================
// Instruction execution
// ===================================================================

static inline i32 imm_i(u32 i) { return (i32)i >> 20; }
static inline i32 imm_s(u32 i) { return ((i32)(i & 0xfe000000) >> 20) | ((i >> 7) & 0x1f); }
static inline i32 imm_b(u32 i)
{
	return ((i32)(i & 0x80000000) >> 19) | ((i & 0x80) << 4) |
	       ((i >> 20) & 0x7e0) | ((i >> 7) & 0x1e);
}
static inline i32 imm_j(u32 i)
{
	return ((i32)(i & 0x80000000) >> 11) | (i & 0xff000) |
	       ((i >> 9) & 0x800) | ((i >> 20) & 0x7fe);
}

[[noreturn]] static void illegal(u32 insn) { throw Trap{EXC_ILLEGAL_INSN, insn}; }

void Cpu::execute(u32 insn)
{
	u32 rd = (insn >> 7) & 31, rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31;
	u32 f3 = (insn >> 12) & 7, f7 = insn >> 25;
	u32 a = x[rs1], b = x[rs2];
	u32 next = pc + 4;

	switch (insn & 0x7f) {
	case 0x37: x[rd] = insn & 0xfffff000; break;                 // LUI
	case 0x17: x[rd] = pc + (insn & 0xfffff000); break;          // AUIPC
	case 0x6f: {                                                 // JAL
		u32 t = pc + imm_j(insn);
		if (t & 3) throw Trap{EXC_INSN_MISALIGNED, t};
		x[rd] = next;
		next = t;
		break;
	}
	case 0x67: {                                                 // JALR
		if (f3) illegal(insn);
		u32 t = (a + imm_i(insn)) & ~1u;
		if (t & 3) throw Trap{EXC_INSN_MISALIGNED, t};
		x[rd] = next;
		next = t;
		break;
	}
	case 0x63: {                                                 // branches
		bool take;
		switch (f3) {
		case 0: take = a == b; break;
		case 1: take = a != b; break;
		case 4: take = (i32)a < (i32)b; break;
		case 5: take = (i32)a >= (i32)b; break;
		case 6: take = a < b; break;
		case 7: take = a >= b; break;
		default: illegal(insn);
		}
		if (take) {
			u32 t = pc + imm_b(insn);
			if (t & 3) throw Trap{EXC_INSN_MISALIGNED, t};
			next = t;
		}
		break;
	}
	case 0x03: {                                                 // loads
		u32 addr = a + imm_i(insn), v;
		switch (f3) {
		case 0: v = (i32)(int8_t)load(addr, 1); break;
		case 1: v = (i32)(int16_t)load(addr, 2); break;
		case 2: v = load(addr, 4); break;
		case 4: v = load(addr, 1); break;
		case 5: v = load(addr, 2); break;
		default: illegal(insn);
		}
		x[rd] = v;
		break;
	}
	case 0x23: {                                                 // stores
		u32 addr = a + imm_s(insn);
		switch (f3) {
		case 0: store(addr, 1, b & 0xff); break;
		case 1: store(addr, 2, b & 0xffff); break;
		case 2: store(addr, 4, b); break;
		default: illegal(insn);
		}
		break;
	}
	case 0x13: {                                                 // OP-IMM
		u32 imm = imm_i(insn), sh = rs2;
		switch (f3) {
		case 0: x[rd] = a + imm; break;
		case 2: x[rd] = (i32)a < (i32)imm; break;
		case 3: x[rd] = a < imm; break;
		case 4: x[rd] = a ^ imm; break;
		case 6: x[rd] = a | imm; break;
		case 7: x[rd] = a & imm; break;
		case 1: if (f7) illegal(insn); x[rd] = a << sh; break;
		case 5:
			if (f7 == 0) x[rd] = a >> sh;
			else if (f7 == 0x20) x[rd] = (i32)a >> sh;
			else illegal(insn);
			break;
		}
		break;
	}
	case 0x33: {                                                 // OP
		if (f7 == 1) {                                       // M extension
			switch (f3) {
			case 0: x[rd] = a * b; break;
			case 1: x[rd] = (u32)(((i64)(i32)a * (i64)(i32)b) >> 32); break;
			case 2: x[rd] = (u32)(((i64)(i32)a * (i64)(u64)b) >> 32); break;
			case 3: x[rd] = (u32)(((u64)a * (u64)b) >> 32); break;
			case 4:
				if (b == 0) x[rd] = ~0u;
				else if (a == 0x80000000u && b == ~0u) x[rd] = a;
				else x[rd] = (i32)a / (i32)b;
				break;
			case 5: x[rd] = b ? a / b : ~0u; break;
			case 6:
				if (b == 0) x[rd] = a;
				else if (a == 0x80000000u && b == ~0u) x[rd] = 0;
				else x[rd] = (i32)a % (i32)b;
				break;
			case 7: x[rd] = b ? a % b : a; break;
			}
			break;
		}
		if (f7 != 0 && !(f7 == 0x20 && (f3 == 0 || f3 == 5))) illegal(insn);
		switch (f3) {
		case 0: x[rd] = f7 ? a - b : a + b; break;
		case 1: x[rd] = a << (b & 31); break;
		case 2: x[rd] = (i32)a < (i32)b; break;
		case 3: x[rd] = a < b; break;
		case 4: x[rd] = a ^ b; break;
		case 5: x[rd] = f7 ? (u32)((i32)a >> (b & 31)) : a >> (b & 31); break;
		case 6: x[rd] = a | b; break;
		case 7: x[rd] = a & b; break;
		}
		break;
	}
	case 0x0f:                                                   // FENCE, FENCE.I
		if (f3 == 1) invalidate_fetch();
		else if (f3 != 0) illegal(insn);
		break;
	case 0x2f: exec_amo(insn); break;
	case 0x73: exec_system(insn); return;   // sets pc itself
	default: illegal(insn);
	}
	pc = next;
}

void Cpu::exec_amo(u32 insn)
{
	u32 rd = (insn >> 7) & 31, rs1 = (insn >> 15) & 31, rs2 = (insn >> 20) & 31;
	u32 f5 = insn >> 27;
	if (((insn >> 12) & 7) != 2) illegal(insn);
	u32 va = x[rs1];
	if (va & 3)
		throw Trap{f5 == 2 ? EXC_LOAD_MISALIGNED : EXC_STORE_MISALIGNED, va};

	if (f5 == 2) {                                       // LR.W
		if (rs2) illegal(insn);
		u64 pa = translate(va, ACC_LOAD);
		x[rd] = phys_read(pa, 4, ACC_LOAD, va);
		resv_valid_ = true;
		resv_addr_ = pa;
		return;
	}
	if (f5 == 3) {                                       // SC.W
		u64 pa = translate(va, ACC_STORE);
		bool ok = resv_valid_ && resv_addr_ == pa;
		resv_valid_ = false;
		if (ok) phys_write(pa, 4, x[rs2], va);
		x[rd] = !ok;
		return;
	}
	// AMOs are read-modify-write: they need write permission and raise
	// store/AMO faults.
	u64 pa = translate(va, ACC_STORE);
	u32 old = phys_read(pa, 4, ACC_STORE, va), b = x[rs2], v;
	switch (f5) {
	case 0x01: v = b; break;                                     // swap
	case 0x00: v = old + b; break;                               // add
	case 0x04: v = old ^ b; break;                               // xor
	case 0x0c: v = old & b; break;                               // and
	case 0x08: v = old | b; break;                               // or
	case 0x10: v = (i32)old < (i32)b ? old : b; break;           // min
	case 0x14: v = (i32)old > (i32)b ? old : b; break;           // max
	case 0x18: v = old < b ? old : b; break;                     // minu
	case 0x1c: v = old > b ? old : b; break;                     // maxu
	default: illegal(insn);
	}
	phys_write(pa, 4, v, va);
	x[rd] = old;
}

void Cpu::exec_system(u32 insn)
{
	u32 rd = (insn >> 7) & 31, rs1 = (insn >> 15) & 31, f3 = (insn >> 12) & 7;
	u32 csr = insn >> 20;

	if (f3 == 0) {
		if (rd) illegal(insn);
		u32 f7 = insn >> 25;
		if (insn == 0x00000073) {                                // ECALL
			if (priv == PRIV_S && cfg_.builtin_sbi) {
				sbi_call();
				pc += 4;
				return;
			}
			throw Trap{priv == PRIV_U ? EXC_ECALL_U : priv == PRIV_S ? EXC_ECALL_S : EXC_ECALL_M, 0};
		}
		if (insn == 0x00100073) throw Trap{EXC_BREAKPOINT, pc};  // EBREAK
		if (insn == 0x30200073) {                                // MRET
			if (priv != PRIV_M) illegal(insn);
			Priv np = (Priv)((mstatus & MS_MPP) >> 11);
			mstatus = (mstatus & ~MS_MIE) | ((mstatus & MS_MPIE) ? MS_MIE : 0);
			mstatus |= MS_MPIE;
			mstatus &= ~MS_MPP;
			if (np != PRIV_M) mstatus &= ~MS_MPRV;
			priv = np;
			pc = mepc_;
			invalidate_fetch();
			return;
		}
		if (insn == 0x10200073) {                                // SRET
			if (priv < PRIV_S || (priv == PRIV_S && (mstatus & MS_TSR))) illegal(insn);
			Priv np = (mstatus & MS_SPP) ? PRIV_S : PRIV_U;
			mstatus = (mstatus & ~MS_SIE) | ((mstatus & MS_SPIE) ? MS_SIE : 0);
			mstatus |= MS_SPIE;
			mstatus &= ~(MS_SPP | MS_MPRV);
			priv = np;
			pc = sepc_;
			invalidate_fetch();
			return;
		}
		if (insn == 0x10500073) {                                // WFI
			if (priv == PRIV_U || (priv == PRIV_S && (mstatus & MS_TW))) illegal(insn);
			wfi();
			pc += 4;
			return;
		}
		if (f7 == 0x09) {                                        // SFENCE.VMA
			if (priv == PRIV_U || (priv == PRIV_S && (mstatus & MS_TVM))) illegal(insn);
			u32 rs2 = (insn >> 20) & 31;
			stats.sfence++;
			if (cfg_.log_sfence) {
				auto &s = stats.fence_sites[{pc, x[1]}];
				s.count++;
				s.has_va = rs1 != 0;
				s.has_asid = rs2 != 0;
			}
			u32 vpn = x[rs1] >> 12, as = x[rs2] & 0x1ff;
			itlb.flush(rs1 != 0, vpn, rs2 != 0, as);
			dtlb.flush(rs1 != 0, vpn, rs2 != 0, as);
			invalidate_fetch();
			pc += 4;
			return;
		}
		illegal(insn);
	}

	// Zicsr
	if (f3 == 4) illegal(insn);
	u32 src = (f3 & 4) ? rs1 : x[rs1];   // immediate forms use rs1 as uimm
	u32 op = f3 & 3;                     // 1 = RW, 2 = RS, 3 = RC
	bool do_write = op == 1 || rs1 != 0;
	csr_check(csr, do_write);
	u32 old = (op == 1 && rd == 0) ? 0 : csr_read(csr);
	if (do_write) {
		u32 nv = op == 1 ? src : op == 2 ? old | src : old & ~src;
		csr_write(csr, nv);
	}
	x[rd] = old;
	pc += 4;
}

// WFI: if nothing is pending, skip time forward to the next timer event
// instead of spinning, so an idle Linux doesn't burn host CPU.
void Cpu::wfi()
{
	if (pending_irqs() & mie_) return;
	if (!bus_.uart.rx.empty()) return;
	u64 next = bus_.clint.mtimecmp;
	if (cfg_.builtin_sbi && sbi_timecmp < next) next = sbi_timecmp;
	u64 now = bus_.clint.mtime;
	if (next > now && next != ~0ull) {
		u64 skip = next - now;
		// Cap the jump so host input still gets polled promptly.
		if (skip > TIMEBASE_HZ / 100) skip = TIMEBASE_HZ / 100;
		bus_.clint.mtime += skip;
		stats.wfi_idle_ticks += skip;
	}
}

// ===================================================================
// CSRs
// ===================================================================

enum : u32 {
	CSR_SSTATUS = 0x100, CSR_SIE = 0x104, CSR_STVEC = 0x105, CSR_SCOUNTEREN = 0x106,
	CSR_SENVCFG = 0x10a, CSR_SSCRATCH = 0x140, CSR_SEPC = 0x141, CSR_SCAUSE = 0x142,
	CSR_STVAL = 0x143, CSR_SIP = 0x144, CSR_SATP = 0x180,
	CSR_MSTATUS = 0x300, CSR_MISA = 0x301, CSR_MEDELEG = 0x302, CSR_MIDELEG = 0x303,
	CSR_MIE = 0x304, CSR_MTVEC = 0x305, CSR_MCOUNTEREN = 0x306, CSR_MENVCFG = 0x30a,
	CSR_MSTATUSH = 0x310, CSR_MENVCFGH = 0x31a, CSR_MCOUNTINHIBIT = 0x320,
	CSR_MSCRATCH = 0x340, CSR_MEPC = 0x341, CSR_MCAUSE = 0x342, CSR_MTVAL = 0x343,
	CSR_MIP = 0x344, CSR_PMPCFG0 = 0x3a0, CSR_PMPADDR0 = 0x3b0,
	CSR_TSELECT = 0x7a0, CSR_TDATA1 = 0x7a1, CSR_TDATA2 = 0x7a2, CSR_TDATA3 = 0x7a3,
	CSR_MCYCLE = 0xb00, CSR_MINSTRET = 0xb02, CSR_MCYCLEH = 0xb80, CSR_MINSTRETH = 0xb82,
	CSR_CYCLE = 0xc00, CSR_TIME = 0xc01, CSR_INSTRET = 0xc02,
	CSR_CYCLEH = 0xc80, CSR_TIMEH = 0xc81, CSR_INSTRETH = 0xc82,
	CSR_MVENDORID = 0xf11, CSR_MARCHID = 0xf12, CSR_MIMPID = 0xf13, CSR_MHARTID = 0xf14,
	CSR_MCONFIGPTR = 0xf15,
};

constexpr u32 SSTATUS_MASK = MS_SIE | MS_SPIE | MS_SPP | MS_SUM | MS_MXR;
constexpr u32 MSTATUS_MASK = SSTATUS_MASK | MS_MIE | MS_MPIE | MS_MPP | MS_MPRV |
	MS_TVM | MS_TW | MS_TSR;
constexpr u32 ALL_IRQS = (1u << IRQ_SSI) | (1u << IRQ_MSI) | (1u << IRQ_STI) |
	(1u << IRQ_MTI) | (1u << IRQ_SEI) | (1u << IRQ_MEI);
constexpr u32 S_IRQS = (1u << IRQ_SSI) | (1u << IRQ_STI) | (1u << IRQ_SEI);
constexpr u32 MISA = (1u << 30) | (1u << 0) | (1u << 8) | (1u << 12) | (1u << 18) | (1u << 20);

// Privilege and read-only checks; counter access gated by [ms]counteren.
void Cpu::csr_check(u32 csr, bool write)
{
	if (priv < ((csr >> 8) & 3)) throw Trap{EXC_ILLEGAL_INSN, 0};
	if (write && (csr >> 10) == 3) throw Trap{EXC_ILLEGAL_INSN, 0};
	if (csr == CSR_SATP && priv == PRIV_S && (mstatus & MS_TVM))
		throw Trap{EXC_ILLEGAL_INSN, 0};
	if ((csr >= 0xc00 && csr <= 0xc1f) || (csr >= 0xc80 && csr <= 0xc9f)) {
		u32 bit = 1u << (csr & 31);
		if (priv < PRIV_M && !(mcounteren & bit)) throw Trap{EXC_ILLEGAL_INSN, 0};
		if (priv < PRIV_S && !(scounteren_ & bit)) throw Trap{EXC_ILLEGAL_INSN, 0};
	}
}

u32 Cpu::csr_read(u32 csr)
{
	u64 cyc = instret + cycle_offset_, ins = instret + instret_offset_;
	switch (csr) {
	case CSR_SSTATUS: return mstatus & SSTATUS_MASK;
	case CSR_SIE: return mie_ & mideleg;
	case CSR_STVEC: return stvec_;
	case CSR_SCOUNTEREN: return scounteren_;
	case CSR_SENVCFG: return senvcfg_;
	case CSR_SSCRATCH: return sscratch_;
	case CSR_SEPC: return sepc_;
	case CSR_SCAUSE: return scause_;
	case CSR_STVAL: return stval_;
	case CSR_SIP: return read_mip() & mideleg;
	case CSR_SATP: return satp_;
	case CSR_MSTATUS: return mstatus;
	case CSR_MISA: return MISA;
	case CSR_MEDELEG: return medeleg;
	case CSR_MIDELEG: return mideleg;
	case CSR_MIE: return mie_;
	case CSR_MTVEC: return mtvec_;
	case CSR_MCOUNTEREN: return mcounteren;
	case CSR_MENVCFG: return menvcfg_;
	case CSR_MSTATUSH: case CSR_MENVCFGH: case CSR_MCOUNTINHIBIT: return 0;
	case CSR_MSCRATCH: return mscratch_;
	case CSR_MEPC: return mepc_;
	case CSR_MCAUSE: return mcause_;
	case CSR_MTVAL: return mtval_;
	case CSR_MIP: return read_mip();
	case CSR_MCYCLE: case CSR_CYCLE: return (u32)cyc;
	case CSR_MCYCLEH: case CSR_CYCLEH: return (u32)(cyc >> 32);
	case CSR_MINSTRET: case CSR_INSTRET: return (u32)ins;
	case CSR_MINSTRETH: case CSR_INSTRETH: return (u32)(ins >> 32);
	case CSR_TIME: return (u32)mtime();
	case CSR_TIMEH: return (u32)(mtime() >> 32);
	case CSR_MVENDORID: case CSR_MARCHID: case CSR_MIMPID: case CSR_MHARTID:
	case CSR_MCONFIGPTR: return 0;
	case CSR_TSELECT: return 1;
	case CSR_TDATA1: case CSR_TDATA2: case CSR_TDATA3: return 0;
	}
	if (csr >= CSR_PMPCFG0 && csr < CSR_PMPCFG0 + 4) return pmpcfg_[csr - CSR_PMPCFG0];
	if (csr >= CSR_PMPADDR0 && csr < CSR_PMPADDR0 + 16) return pmpaddr_[csr - CSR_PMPADDR0];
	// Unimplemented hardware performance counters read as zero.
	if ((csr >= 0xb03 && csr <= 0xb1f) || (csr >= 0xb83 && csr <= 0xb9f) ||
	    (csr >= 0xc03 && csr <= 0xc1f) || (csr >= 0xc83 && csr <= 0xc9f) ||
	    (csr >= 0x323 && csr <= 0x33f))
		return 0;
	throw Trap{EXC_ILLEGAL_INSN, 0};
}

void Cpu::csr_write(u32 csr, u32 v)
{
	switch (csr) {
	case CSR_SSTATUS:
		mstatus = (mstatus & ~SSTATUS_MASK) | (v & SSTATUS_MASK);
		return;
	case CSR_SIE: mie_ = (mie_ & ~mideleg) | (v & mideleg); return;
	case CSR_STVEC: stvec_ = v & ~2u; return;
	case CSR_SCOUNTEREN: scounteren_ = v; return;
	case CSR_SENVCFG: senvcfg_ = 0; return;
	case CSR_SSCRATCH: sscratch_ = v; return;
	case CSR_SEPC: sepc_ = v & ~3u; return;
	case CSR_SCAUSE: scause_ = v; return;
	case CSR_STVAL: stval_ = v; return;
	case CSR_SIP:
		// Only SSIP is writable from S-mode, and only if delegated.
		mip_sw_ = (mip_sw_ & ~(mideleg & (1u << IRQ_SSI))) | (v & mideleg & (1u << IRQ_SSI));
		return;
	case CSR_SATP: {
		u32 mode = v >> 31;
		u32 asid_mask = cfg_.asid_bits ? 0x1ff : 0;   // WARL: unsupported ASID bits read 0
		satp_ = (mode << 31) | (v & (asid_mask << 22)) | (v & 0x3fffff);
		stats.satp_writes++;
		invalidate_fetch();
		return;
	}
	case CSR_MSTATUS: {
		u32 nv = (mstatus & ~MSTATUS_MASK) | (v & MSTATUS_MASK);
		if (((nv & MS_MPP) >> 11) == 2)                // reserved: keep old MPP
			nv = (nv & ~MS_MPP) | (mstatus & MS_MPP);
		mstatus = nv;
		return;
	}
	case CSR_MISA: return;
	case CSR_MEDELEG: medeleg = v & 0xb3ff; return;  // never delegate M-ecall
	case CSR_MIDELEG: mideleg = v & S_IRQS; return;
	case CSR_MIE: mie_ = v & ALL_IRQS; return;
	case CSR_MTVEC: mtvec_ = v & ~2u; return;
	case CSR_MCOUNTEREN: mcounteren = v; return;
	case CSR_MENVCFG: menvcfg_ = 0; return;
	case CSR_MSTATUSH: case CSR_MENVCFGH: case CSR_MCOUNTINHIBIT: return;
	case CSR_MSCRATCH: mscratch_ = v; return;
	case CSR_MEPC: mepc_ = v & ~3u; return;
	case CSR_MCAUSE: mcause_ = v; return;
	case CSR_MTVAL: mtval_ = v; return;
	case CSR_MIP: {
		u32 m = (1u << IRQ_SSI) | (1u << IRQ_STI) | (1u << IRQ_SEI);
		mip_sw_ = (mip_sw_ & ~m) | (v & m);
		return;
	}
	// Writing a counter suppresses its increment for the writing instruction:
	// the next instruction reads exactly the value written. This instruction
	// will still bump `instret` when it retires, hence the +1.
	case CSR_MCYCLE: cycle_offset_ = set_lo(cycle_offset_, v); return;
	case CSR_MCYCLEH: cycle_offset_ = set_hi(cycle_offset_, v); return;
	case CSR_MINSTRET: instret_offset_ = set_lo(instret_offset_, v); return;
	case CSR_MINSTRETH: instret_offset_ = set_hi(instret_offset_, v); return;
	// Debug triggers (Sdtrig) are not implemented: tselect is hard-wired so
	// software sees that trigger 0 doesn't exist.
	case CSR_TSELECT: case CSR_TDATA1: case CSR_TDATA2: case CSR_TDATA3: return;
	}
	if (csr >= CSR_PMPCFG0 && csr < CSR_PMPCFG0 + 4) { pmpcfg_[csr - CSR_PMPCFG0] = v; return; }
	if (csr >= CSR_PMPADDR0 && csr < CSR_PMPADDR0 + 16) { pmpaddr_[csr - CSR_PMPADDR0] = v; return; }
	csr_read(csr);   // throws if the CSR doesn't exist; otherwise ignore the write
}

void Cpu::dump_state(FILE *f) const
{
	static const char *names[32] = {"zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
		"s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3",
		"s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};
	fprintf(f, "pc=%08x priv=%c instret=%llu mstatus=%08x satp=%08x\n", pc,
		"US?M"[priv], (unsigned long long)instret, mstatus, satp_);
	fprintf(f, "scause=%08x sepc=%08x stval=%08x stvec=%08x\n", scause_, sepc_, stval_, stvec_);
	fprintf(f, "mcause=%08x mepc=%08x mtval=%08x mtvec=%08x\n", mcause_, mepc_, mtval_, mtvec_);
	for (int i = 0; i < 32; i++)
		fprintf(f, "%4s=%08x%s", names[i], x[i], i % 4 == 3 ? "\n" : "  ");
}
