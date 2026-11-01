// bootlinux: an RV32IMA full-system emulator that boots Linux.
#include "cpu.h"
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <termios.h>
#include <unistd.h>
#include <vector>

static void usage()
{
	fprintf(stderr,
"usage: bootlinux [options] KERNEL_IMAGE        boot Linux (S-mode, built-in SBI)\n"
"       bootlinux --elf TEST.elf [options]      run a bare-metal ELF in M-mode\n"
"options:\n"
"  --dtb FILE         device tree blob (default build/bootlinux.dtb)\n"
"  --ram MB           RAM size in MiB (default 64; must match the DTB)\n"
"  --itlb N           instruction TLB entries (default 32)\n"
"  --dtlb N           data TLB entries (default 32)\n"
"  --asid-bits N      0 or 9 (default 9)\n"
"  --split-superpages TLB caches 4 MiB pages as 4 KiB entries\n"
"  --sfence-log FILE  write sfence.vma call sites (pc, ra, count)\n"
"  --script FILE      type each line of FILE at the shell prompt, then exit\n"
"  --max-insns N      stop after N instructions\n"
"  --stats FILE       write statistics as JSON\n"
"  --quiet            don't echo guest console output\n"
"  --trace N          print the first N executed instructions\n"
"interactive keys: Ctrl-A x = quit\n");
	exit(2);
}

static std::vector<u8> read_file(const std::string &path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f) fatal("cannot open " + path);
	return std::vector<u8>((std::istreambuf_iterator<char>(f)), {});
}

// Minimal ELF32 loader: copies PT_LOAD segments, finds the `tohost` symbol.
static u32 load_elf(Bus &bus, const std::vector<u8> &f, u64 &tohost)
{
	auto rd16 = [&](size_t o) { u16 v; memcpy(&v, &f[o], 2); return v; };
	auto rd32 = [&](size_t o) { u32 v; memcpy(&v, &f[o], 4); return v; };
	if (f.size() < 52 || memcmp(f.data(), "\x7f" "ELF", 4) || f[4] != 1)
		fatal("not an ELF32 file");
	u32 entry = rd32(24), phoff = rd32(28), shoff = rd32(32);
	u16 phnum = rd16(44), shnum = rd16(48);
	for (int i = 0; i < phnum; i++) {
		size_t ph = phoff + i * 32;
		if (rd32(ph) != 1) continue;                    // PT_LOAD
		u32 off = rd32(ph + 4), pa = rd32(ph + 12), fsz = rd32(ph + 16), msz = rd32(ph + 20);
		if (!bus.in_ram(pa, msz)) fatal("ELF segment outside RAM");
		memcpy(&bus.ram[pa - RAM_BASE], &f[off], fsz);
		memset(&bus.ram[pa - RAM_BASE + fsz], 0, msz - fsz);
	}
	for (int i = 0; i < shnum; i++) {                     // find .symtab
		size_t sh = shoff + i * 40;
		if (rd32(sh + 4) != 2) continue;                // SHT_SYMTAB
		u32 off = rd32(sh + 16), size = rd32(sh + 20), link = rd32(sh + 24);
		u32 stroff = rd32(shoff + link * 40 + 16);
		for (u32 s = off; s < off + size; s += 16)
			if (!strcmp((const char *)&f[stroff + rd32(s)], "tohost"))
				tohost = rd32(s + 4);
	}
	return entry;
}

// ----- host terminal
static termios saved_tio;
static bool tio_raw = false;
static void restore_tty() { if (tio_raw) tcsetattr(0, TCSANOW, &saved_tio); }

static void raw_tty()
{
	if (!isatty(0)) return;
	tcgetattr(0, &saved_tio);
	termios t = saved_tio;
	t.c_lflag &= ~(ICANON | ECHO | ISIG);
	t.c_iflag &= ~(IXON | ICRNL);
	t.c_cc[VMIN] = 0;
	t.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &t);
	tio_raw = true;
	atexit(restore_tty);
}

static void report(const Cpu &c, double secs, FILE *f, bool json);

int main(int argc, char **argv)
{
	std::string image, elf, dtb = "build/bootlinux.dtb", script, stats_path, sfence_path;
	u32 ram_mb = 64;
	u64 max_insns = ~0ull, trace = 0;
	bool quiet = false;
	CpuConfig cfg;

	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&]() -> std::string { if (i + 1 >= argc) usage(); return argv[++i]; };
		if (a == "--elf") elf = next();
		else if (a == "--dtb") dtb = next();
		else if (a == "--ram") ram_mb = std::stoul(next());
		else if (a == "--itlb") cfg.itlb_entries = std::stoi(next());
		else if (a == "--dtlb") cfg.dtlb_entries = std::stoi(next());
		else if (a == "--asid-bits") cfg.asid_bits = std::stoi(next());
		else if (a == "--split-superpages") cfg.split_superpages = true;
		else if (a == "--sfence-log") { sfence_path = next(); cfg.log_sfence = true; }
		else if (a == "--script") script = next();
		else if (a == "--max-insns") max_insns = std::stoull(next());
		else if (a == "--stats") stats_path = next();
		else if (a == "--quiet") quiet = true;
		else if (a == "--trace") trace = std::stoull(next());
		else if (a[0] == '-') usage();
		else image = a;
	}
	if (image.empty() == elf.empty()) usage();
	if (cfg.asid_bits != 0 && cfg.asid_bits != 9) fatal("--asid-bits must be 0 or 9");

	Bus bus(ram_mb << 20);
	cfg.builtin_sbi = elf.empty();
	Cpu cpu(bus, cfg);
	cpu.trace_left = trace;
	if (quiet && !freopen("/dev/null", "w", stdout)) fatal("cannot silence stdout");

	if (!elf.empty()) {
		// Bare-metal test: start in M-mode at the ELF entry point.
		u64 tohost = 0;
		u32 entry = load_elf(bus, read_file(elf), tohost);
		cpu.tohost_addr = tohost;
		cpu.reset(entry, PRIV_M);
	} else {
		// Linux: kernel at the start of RAM, DTB near the top. Enter the
		// kernel in S-mode as SBI firmware would: a0 = hart id, a1 = DTB.
		auto k = read_file(image), d = read_file(dtb);
		if (k.size() > bus.ram_size / 2) fatal("kernel too large for RAM");
		memcpy(bus.ram.data(), k.data(), k.size());
		u32 dtb_pa = RAM_BASE + bus.ram_size - 0x100000;
		memcpy(&bus.ram[dtb_pa - RAM_BASE], d.data(), d.size());
		cpu.reset(RAM_BASE, PRIV_S);
		cpu.x[10] = 0;
		cpu.x[11] = dtb_pa;
		// What OpenSBI would have set up: delegate S-level interrupts and
		// every exception Linux handles itself, and let S-mode read time.
		cpu.mideleg = (1u << IRQ_SSI) | (1u << IRQ_STI) | (1u << IRQ_SEI);
		cpu.medeleg = (1u << EXC_INSN_MISALIGNED) | (1u << EXC_INSN_ACCESS) |
			(1u << EXC_ILLEGAL_INSN) | (1u << EXC_BREAKPOINT) |
			(1u << EXC_LOAD_MISALIGNED) | (1u << EXC_LOAD_ACCESS) |
			(1u << EXC_STORE_MISALIGNED) | (1u << EXC_STORE_ACCESS) |
			(1u << EXC_ECALL_U) | (1u << EXC_INSN_PAGE_FAULT) |
			(1u << EXC_LOAD_PAGE_FAULT) | (1u << EXC_STORE_PAGE_FAULT);
		cpu.mcounteren = 7;
	}

	// Scripted input: one line per shell prompt, then Ctrl-D (EOF).
	std::vector<std::string> lines;
	size_t next_line = 0;
	bool scripted = !script.empty();
	if (scripted) {
		auto text = read_file(script);
		std::istringstream in(std::string(text.begin(), text.end()));
		for (std::string l; std::getline(in, l);)
			if (!l.empty() && l[0] != '#') lines.push_back(l);
	} else if (elf.empty()) {
		raw_tty();
	}

	auto t0 = std::chrono::steady_clock::now();
	bool ctrl_a = false;
	while (cpu.stop == StopReason::None && cpu.instret < max_insns) {
		cpu.run(std::min<u64>(20000, max_insns - cpu.instret));

		Uart &u = bus.uart;
		if (scripted) {
			const std::string &log = u.tx_log;
			if (u.rx.empty() && log.size() >= 2 && log.compare(log.size() - 2, 2, "# ") == 0) {
				std::string l = next_line < lines.size() ? lines[next_line++] + "\n" : "\x04";
				u.rx.insert(u.rx.end(), l.begin(), l.end());
				u.tx_log.clear();
				cpu.refresh_ext();
			}
		} else if (tio_raw) {
			char c;
			while (read(0, &c, 1) == 1) {
				if (ctrl_a && c == 'x') { cpu.stop = StopReason::Shutdown; break; }
				ctrl_a = c == 0x01;
				if (!ctrl_a) u.rx.push_back(c == '\x7f' ? '\b' : c);
				cpu.refresh_ext();
			}
		}
	}
	double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	restore_tty();
	fflush(stdout);

	if (!elf.empty()) {
		bool pass = cpu.stop == StopReason::ToHost && cpu.exit_code == 0;
		fprintf(stderr, "%s: %s", elf.c_str(), pass ? "PASS\n" : "FAIL");
		if (!pass) {
			if (cpu.stop == StopReason::ToHost) fprintf(stderr, " (test %u)\n", cpu.exit_code);
			else fprintf(stderr, " (no result after %llu instructions)\n", (unsigned long long)cpu.instret);
		}
		return pass ? 0 : 1;
	}

	report(cpu, secs, stderr, false);
	if (!stats_path.empty()) {
		FILE *f = fopen(stats_path.c_str(), "w");
		if (!f) fatal("cannot write " + stats_path);
		report(cpu, secs, f, true);
		fclose(f);
	}
	if (!sfence_path.empty()) {
		FILE *f = fopen(sfence_path.c_str(), "w");
		if (!f) fatal("cannot write " + sfence_path);
		fprintf(f, "# pc ra count has_va has_asid\n");
		for (auto &[k, v] : cpu.stats.fence_sites)
			fprintf(f, "%08x %08x %llu %d %d\n", k.first, k.second,
				(unsigned long long)v.count, v.has_va, v.has_asid);
		fclose(f);
	}
	return cpu.stop == StopReason::Fatal ? 1 : 0;
}

static double pct(u64 a, u64 b) { return b ? 100.0 * a / b : 0; }

static void report(const Cpu &c, double secs, FILE *f, bool json)
{
	const CpuStats &s = c.stats;
	const TlbStats &it = c.itlb.stats, &dt = c.dtlb.stats;
	u64 total = c.instret;
	u64 exc = 0, irq = 0;
	for (int i = 0; i < 16; i++) { exc += s.exceptions[i]; irq += s.interrupts[i]; }

	if (json) {
		fprintf(f, "{\n  \"instructions\": %llu,\n", (unsigned long long)total);
		fprintf(f, "  \"insns_m\": %llu, \"insns_s\": %llu, \"insns_u\": %llu,\n",
			(unsigned long long)s.insns[3], (unsigned long long)s.insns[1], (unsigned long long)s.insns[0]);
		fprintf(f, "  \"itlb_entries\": %d, \"dtlb_entries\": %d,\n", c.itlb.size(), c.dtlb.size());
		fprintf(f, "  \"itlb_hits\": %llu, \"itlb_misses\": %llu,\n",
			(unsigned long long)it.hits, (unsigned long long)it.misses);
		fprintf(f, "  \"dtlb_hits\": %llu, \"dtlb_misses\": %llu,\n",
			(unsigned long long)dt.hits, (unsigned long long)dt.misses);
		fprintf(f, "  \"dtlb_super_hits\": %llu, \"itlb_super_hits\": %llu,\n",
			(unsigned long long)dt.super_hits, (unsigned long long)it.super_hits);
		fprintf(f, "  \"pte_reads\": %llu, \"ad_updates\": %llu,\n",
			(unsigned long long)s.pte_reads, (unsigned long long)s.ad_updates);
		fprintf(f, "  \"sfence\": %llu, \"satp_writes\": %llu,\n",
			(unsigned long long)s.sfence, (unsigned long long)s.satp_writes);
		fprintf(f, "  \"flush_all\": %llu, \"flush_page\": %llu, \"flush_asid\": %llu,\n",
			(unsigned long long)dt.flush_all, (unsigned long long)dt.flush_page,
			(unsigned long long)dt.flush_asid);
		fprintf(f, "  \"page_faults\": %llu, \"exceptions\": %llu, \"interrupts\": %llu,\n",
			(unsigned long long)(s.exceptions[12] + s.exceptions[13] + s.exceptions[15]),
			(unsigned long long)exc, (unsigned long long)irq);
		fprintf(f, "  \"syscalls\": %llu, \"split_fills\": %llu, \"walks_fetch\": %llu,\n",
			(unsigned long long)s.exceptions[8], (unsigned long long)s.split_fills,
			(unsigned long long)s.walks[0]);
		fprintf(f, "  \"walks_load\": %llu, \"walks_store\": %llu,\n",
			(unsigned long long)s.walks[1], (unsigned long long)s.walks[2]);
		fprintf(f, "  \"sbi_calls\": %llu, \"seconds\": %.3f\n}\n",
			(unsigned long long)s.sbi_calls, secs);
		return;
	}

	fprintf(f, "\n---- bootlinux statistics ----\n");
	fprintf(f, "instructions   %12llu  (%.1f MIPS host speed)\n",
		(unsigned long long)total, secs > 0 ? total / secs / 1e6 : 0);
	fprintf(f, "  user / super / machine: %.1f%% / %.1f%% / %.1f%%\n",
		pct(s.insns[0], total), pct(s.insns[1], total), pct(s.insns[3], total));
	fprintf(f, "ITLB (%3d)     %12llu lookups, %.3f%% miss\n", c.itlb.size(),
		(unsigned long long)(it.hits + it.misses), pct(it.misses, it.hits + it.misses));
	fprintf(f, "DTLB (%3d)     %12llu lookups, %.3f%% miss, %.1f%% of hits on 4 MiB pages\n",
		c.dtlb.size(), (unsigned long long)(dt.hits + dt.misses),
		pct(dt.misses, dt.hits + dt.misses), pct(dt.super_hits, dt.hits));
	fprintf(f, "page walks     %12llu  (fetch %llu, load %llu, store %llu), %llu PTE reads\n",
		(unsigned long long)(s.walks[0] + s.walks[1] + s.walks[2]),
		(unsigned long long)s.walks[0], (unsigned long long)s.walks[1],
		(unsigned long long)s.walks[2], (unsigned long long)s.pte_reads);
	fprintf(f, "A/D updates    %12llu\n", (unsigned long long)s.ad_updates);
	fprintf(f, "sfence.vma     %12llu  (all %llu, page %llu, asid %llu), satp writes %llu\n",
		(unsigned long long)s.sfence, (unsigned long long)dt.flush_all,
		(unsigned long long)dt.flush_page, (unsigned long long)dt.flush_asid,
		(unsigned long long)s.satp_writes);
	fprintf(f, "page faults    %12llu  (fetch %llu, load %llu, store %llu)\n",
		(unsigned long long)(s.exceptions[12] + s.exceptions[13] + s.exceptions[15]),
		(unsigned long long)s.exceptions[12], (unsigned long long)s.exceptions[13],
		(unsigned long long)s.exceptions[15]);
	fprintf(f, "syscalls       %12llu\n", (unsigned long long)s.exceptions[8]);
	fprintf(f, "interrupts     %12llu  (timer %llu, external %llu)\n", (unsigned long long)irq,
		(unsigned long long)s.interrupts[IRQ_STI], (unsigned long long)s.interrupts[IRQ_SEI]);
	fprintf(f, "SBI calls      %12llu\n", (unsigned long long)s.sbi_calls);
}
