// Built-in SBI (Supervisor Binary Interface) firmware.
//
// On real hardware, Linux runs in S-mode on top of M-mode firmware such as
// OpenSBI, and asks it for services with `ecall`: set the timer, print to
// the early console, shut down. Here the emulator answers those calls
// directly in C++, so no M-mode firmware binary is needed.
#include "cpu.h"

namespace {
enum : u32 {
	EXT_LEGACY_PUTCHAR = 0x01,
	EXT_LEGACY_GETCHAR = 0x02,
	EXT_BASE = 0x10,
	EXT_TIME = 0x54494d45,  // "TIME"
	EXT_IPI = 0x00735049,   // "sPI"
	EXT_RFENCE = 0x52464e43,// "RFNC"
	EXT_HSM = 0x0048534d,   // "HSM"
	EXT_SRST = 0x53525354,  // "SRST"
	EXT_DBCN = 0x4442434e,  // "DBCN" debug console
};
constexpr i32 SBI_SUCCESS = 0, SBI_ERR_NOT_SUPPORTED = -2, SBI_ERR_INVALID_PARAM = -3;
}

void Cpu::sbi_call()
{
	stats.sbi_calls++;
	u32 ext = x[17], fid = x[16];
	i32 err = SBI_SUCCESS;
	u32 val = 0;

	switch (ext) {
	case EXT_LEGACY_PUTCHAR:
		putchar((int)x[10]);
		fflush(stdout);
		x[10] = 0;
		return;
	case EXT_LEGACY_GETCHAR:
		x[10] = ~0u;
		return;

	case EXT_BASE:
		switch (fid) {
		case 0: val = 2u << 24; break;          // spec version 2.0
		case 1: val = 0xb007; break;            // implementation ID (unofficial)
		case 2: val = 1; break;                 // implementation version
		case 3:                                 // probe_extension
			switch (x[10]) {
			case EXT_TIME: case EXT_IPI: case EXT_RFENCE: case EXT_HSM:
			case EXT_SRST: case EXT_DBCN: val = 1; break;
			default: val = 0;
			}
			break;
		case 4: case 5: case 6: val = 0; break; // mvendorid/marchid/mimpid
		default: err = SBI_ERR_NOT_SUPPORTED;
		}
		break;

	case EXT_TIME:
		if (fid != 0) { err = SBI_ERR_NOT_SUPPORTED; break; }
		// set_timer(stime_value): 64-bit value in a0 (low) / a1 (high).
		// Pending STIP is derived from mtime >= sbi_timecmp, so writing a
		// future value also clears it.
		sbi_timecmp = ((u64)x[11] << 32) | x[10];
		break;

	case EXT_IPI:
	case EXT_RFENCE:
		// Single hart: there are no other harts to interrupt or fence.
		// Linux fences its own hart locally before making these calls.
		break;

	case EXT_HSM:
		if (fid == 2) val = 0;                  // hart_get_status: STARTED
		else err = SBI_ERR_NOT_SUPPORTED;
		break;

	case EXT_SRST:
		if (fid != 0) { err = SBI_ERR_NOT_SUPPORTED; break; }
		// system_reset(type, reason): 0 = shutdown, 1/2 = reboot
		stop = x[10] == 0 ? StopReason::Shutdown : StopReason::Reboot;
		exit_code = x[11];
		break;

	case EXT_DBCN:
		switch (fid) {
		case 0: {                               // write(len, addr_lo, addr_hi)
			u64 pa = ((u64)x[12] << 32) | x[11];
			u32 len = x[10];
			if (!bus_.in_ram(pa, len)) { err = SBI_ERR_INVALID_PARAM; break; }
			fwrite(&bus_.ram[pa - RAM_BASE], 1, len, stdout);
			fflush(stdout);
			val = len;
			break;
		}
		case 1: val = 0; break;                 // read: nothing available
		case 2: putchar((int)(x[10] & 0xff)); fflush(stdout); break;
		default: err = SBI_ERR_NOT_SUPPORTED;
		}
		break;

	default:
		err = SBI_ERR_NOT_SUPPORTED;
	}
	x[10] = (u32)err;
	x[11] = val;
}
