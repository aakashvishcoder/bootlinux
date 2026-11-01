// A modeled translation lookaside buffer.
//
// This TLB is not just a statistics counter: every address translation in
// the emulator goes through it, and on a hit the cached PTE is used without
// re-reading the page table. So if the emulator implemented sfence.vma or
// ASIDs wrongly, Linux would read stale translations and crash. That makes
// the hit/miss numbers trustworthy: they come from a TLB the OS actually
// depends on.
//
// Organization: fully associative, true LRU, configurable size. Entries can
// map a 4 KiB page or a 4 MiB Sv32 superpage, and are tagged with an ASID
// (unless the PTE is global) so a context switch need not flush.
#pragma once
#include "common.h"
#include <vector>

struct TlbEntry {
	bool valid = false;
	bool super = false;   // 4 MiB megapage (matches on VPN[1] only)
	bool global = false;  // G bit: matches every ASID
	u32 vpn = 0;          // full 20-bit VPN (VPN[0] ignored if super)
	u32 asid = 0;
	u32 ppn = 0;          // 22-bit PPN of the page (or megapage base)
	u32 pte = 0;          // cached PTE flag bits (V R W X U G A D)
	u32 pte_addr = 0;     // where the leaf PTE lives, for A/D updates
	u64 last_use = 0;
};

struct TlbStats {
	u64 hits = 0, misses = 0, evictions = 0;
	u64 flush_all = 0, flush_page = 0, flush_asid = 0;
	u64 super_hits = 0;
};

class Tlb {
public:
	explicit Tlb(int entries) : e_(entries > 0 ? entries : 1) {}

	int size() const { return (int)e_.size(); }

	// Look up a virtual page number; returns nullptr on a miss.
	TlbEntry *lookup(u32 vpn, u32 asid)
	{
		// Fast check of the most recently used entry first: most accesses
		// hit the same page as the previous one.
		if (last_ && match(*last_, vpn, asid)) return hit(last_);
		for (auto &t : e_)
			if (match(t, vpn, asid)) return hit(&t);
		stats.misses++;
		return nullptr;
	}

	// Install a translation, evicting the least recently used entry.
	TlbEntry *insert(const TlbEntry &n)
	{
		TlbEntry *victim = &e_[0];
		for (auto &t : e_) {
			if (!t.valid) { victim = &t; break; }
			if (t.last_use < victim->last_use) victim = &t;
		}
		if (victim->valid) stats.evictions++;
		*victim = n;
		victim->valid = true;
		victim->last_use = ++clock_;
		last_ = victim;
		epoch++;
		return victim;
	}

	// sfence.vma rs1, rs2 semantics. has_va: rs1 != x0, has_asid: rs2 != x0.
	void flush(bool has_va, u32 vpn, bool has_asid, u32 asid)
	{
		if (!has_va && !has_asid) stats.flush_all++;
		else if (has_va) stats.flush_page++;
		else stats.flush_asid++;
		for (auto &t : e_) {
			if (!t.valid) continue;
			if (has_va && !covers(t, vpn)) continue;
			// An ASID-specific fence leaves global mappings alone.
			if (has_asid && (t.global || t.asid != asid)) continue;
			t.valid = false;
		}
		last_ = nullptr;
		epoch++;
	}

	TlbStats stats;
	u64 epoch = 0;   // bumps on any change; lets callers cache a lookup

private:
	static bool covers(const TlbEntry &t, u32 vpn)
	{
		return t.super ? (t.vpn >> 10) == (vpn >> 10) : t.vpn == vpn;
	}
	static bool match(const TlbEntry &t, u32 vpn, u32 asid)
	{
		return t.valid && covers(t, vpn) && (t.global || t.asid == asid);
	}
	TlbEntry *hit(TlbEntry *t)
	{
		stats.hits++;
		if (t->super) stats.super_hits++;
		t->last_use = ++clock_;
		last_ = t;
		return t;
	}

	std::vector<TlbEntry> e_;
	TlbEntry *last_ = nullptr;
	u64 clock_ = 0;
};
