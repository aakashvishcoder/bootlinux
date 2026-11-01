// RV32IMA hart with Zicsr/Zifencei, M/S/U privilege modes and Sv32 paging.
#pragma once
#include "common.h"
#include "devices.h"
#include "tlb.h"
#include <map>
#include <utility>

// mstatus bits
constexpr u32 MS_SIE = 1u << 1, MS_MIE = 1u << 3, MS_SPIE = 1u << 5,
	MS_MPIE = 1u << 7, MS_SPP = 1u << 8, MS_MPP = 3u << 11,
	MS_MPRV = 1u << 17, MS_SUM = 1u << 18, MS_MXR = 1u << 19,
	MS_TVM = 1u << 20, MS_TW = 1u << 21, MS_TSR = 1u << 22;

// Sv32 PTE bits
constexpr u32 PTE_V = 1, PTE_R = 2, PTE_W = 4, PTE_X = 8, PTE_U = 16,
	PTE_G = 32, PTE_A = 64, PTE_D = 128;

struct CpuConfig {
	int itlb_entries = 32;
	int dtlb_entries = 32;
	int asid_bits = 9;        // 0 disables ASIDs (every satp switch needs a flush)
	bool builtin_sbi = false; // emulate SBI firmware for S-mode ecalls
	bool split_superpages = false; // cache 4 MiB pages as 4 KiB TLB entries
	bool log_sfence = false;  // record which code issues sfence.vma
};

struct CpuStats {
	u64 insns[4] = {};          // retired instructions by privilege level
	u64 exceptions[16] = {};
	u64 interrupts[16] = {};
	u64 walks[3] = {};          // page-table walks by access kind
	u64 pte_reads = 0;
	u64 ad_updates = 0;         // hardware Accessed/Dirty bit writes
	u64 satp_writes = 0;
	u64 sfence = 0;
	u64 sbi_calls = 0;
	u64 wfi_idle_ticks = 0;     // timer ticks skipped while waiting in WFI
	u64 split_fills = 0;        // superpage walks cached as a 4 KiB entry
	// sfence.vma call sites: (pc, return address) -> count, kind
	struct FenceSite { u64 count = 0; bool has_va = false, has_asid = false; };
	std::map<std::pair<u32, u32>, FenceSite> fence_sites;
};

enum class StopReason { None, Shutdown, Reboot, ToHost, Fatal };

class Cpu {
public:
	Cpu(Bus &bus, const CpuConfig &cfg);

	void reset(u32 pc, Priv priv);
	void run(u64 max_insns);      // runs until stop or max_insns
	void step();

	u32 x[32] = {};
	u32 pc = 0;
	Priv priv = PRIV_M;

	StopReason stop = StopReason::None;
	u32 exit_code = 0;
	u64 tohost_addr = 0;          // riscv-tests completion address (0 = none)
	u64 trace_left = 0;           // print this many instructions

	CpuStats stats;
	Tlb itlb, dtlb;
	u64 instret = 0;
	u64 sbi_timecmp = ~0ull;

	// External interrupt lines from the PLIC; recomputed after device access.
	void refresh_ext();
	void dump_state(FILE *f) const;

	// CSRs exposed for the loader / SBI.
	u32 mstatus = 0, medeleg = 0, mideleg = 0, mcounteren = 0;

private:
	Bus &bus_;
	CpuConfig cfg_;

	// Remaining CSRs
	u32 mie_ = 0, mip_sw_ = 0, mtvec_ = 0, mscratch_ = 0, mepc_ = 0,
	    mcause_ = 0, mtval_ = 0, menvcfg_ = 0;
	u32 stvec_ = 0, sscratch_ = 0, sepc_ = 0, scause_ = 0, stval_ = 0,
	    satp_ = 0, scounteren_ = 0, senvcfg_ = 0;
	u32 pmpcfg_[4] = {}, pmpaddr_[16] = {};
	u64 cycle_offset_ = 0, instret_offset_ = 0;
	// New counter offset so that, after the current instruction retires,
	// the counter reads back with its low / high word replaced by v.
	u64 set_lo(u64 off, u32 v) const
	{
		u64 cur = instret + off;
		return ((cur & ~0xffffffffull) | v) - (instret + 1);
	}
	u64 set_hi(u64 off, u32 v) const
	{
		u64 cur = instret + off;
		return (((u64)v << 32) | (u32)cur) - (instret + 1);
	}
	bool ext_m_ = false, ext_s_ = false;
	int tick_count_ = 0;

	// LR/SC reservation (physical address)
	bool resv_valid_ = false;
	u64 resv_addr_ = 0;

	// Cache of the current instruction page (host pointer), so sequential
	// fetches don't repeat the translation. Exact w.r.t. the ITLB model:
	// see fetch().
	u32 fetch_vpage_ = 1;         // odd value = invalid (pages are aligned)
	u8 *fetch_host_ = nullptr;
	u64 fetch_epoch_ = 0;
	bool fetch_tlb_ = false;
	void invalidate_fetch() { fetch_vpage_ = 1; }

	// Execution
	u32 fetch();
	void execute(u32 insn);
	void exec_system(u32 insn);
	void exec_amo(u32 insn);
	bool check_interrupts();
	u32 pending_irqs() const;
	void take_trap(u32 cause, u32 tval, bool interrupt);
	void sbi_call();
	void wfi();

	// Memory
	u64 translate(u32 va, Access acc);
	TlbEntry *walk(u32 va, Access acc, Priv eff, Tlb &tlb);
	u32 load(u32 va, int len);
	void store(u32 va, int len, u32 val);
	u32 phys_read(u64 pa, int len, Access acc, u32 va);
	void phys_write(u64 pa, int len, u32 val, u32 va);
	u32 asid() const { return cfg_.asid_bits ? (satp_ >> 22) & 0x1ff : 0; }

	// CSRs
	u32 csr_read(u32 csr);
	void csr_write(u32 csr, u32 val);
	void csr_check(u32 csr, bool write);
	u32 read_mip() const { return pending_irqs(); }
	u64 mtime() const { return bus_.clint.mtime; }
};
