#!/usr/bin/env bash
# Build and run the official riscv-tests on the emulator.
#   RISCV_TESTS=/path/to/riscv-tests tests/run_riscv_tests.sh [filter]
#
# "p" tests run in physical-address mode. "v" tests run the same programs
# in user mode under a small kernel that demand-pages them through Sv32,
# so they also exercise the MMU, page faults and trap delegation.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=${RISCV_TESTS:?set RISCV_TESTS to a riscv-tests checkout (with env/)}
CC=${CC_RV:-riscv64-linux-gnu-gcc}
OUT=$ROOT/build/tests
EMU=$ROOT/build/bootlinux
FILTER=${1:-}
mkdir -p "$OUT"

ARCH="-march=rv32ima_zicsr_zifencei -mabi=ilp32"
COMMON="$ARCH -static -mcmodel=medany -fvisibility=hidden -nostdlib -nostartfiles"

build_p() {   # suite name
	$CC $COMMON -I "$T/env/p" -I "$T/isa/macros/scalar" -T "$T/env/p/link.ld" \
		"$T/isa/$1/$2.S" -o "$OUT/$1-p-$2" 2>/dev/null
}
build_v() {
	$CC $COMMON -std=gnu99 -O2 -DENTROPY=0x$(echo "$1-$2" | md5sum | cut -c1-7) \
		-I "$T/env/v" -I "$T/isa/macros/scalar" -T "$T/env/v/link.ld" \
		"$T/env/v/entry.S" "$T/env/v/"*.c "$T/isa/$1/$2.S" -o "$OUT/$1-v-$2" 2>/dev/null
}

pass=0 fail=0 skipped=0 failed=()
run() {   # binary name
	if [ ! -f "$OUT/$1" ]; then skipped=$((skipped + 1)); return; fi
	if timeout 20 "$EMU" --elf "$OUT/$1" --max-insns 50000000 >/dev/null 2>&1; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1)); failed+=("$1")
	fi
}

for suite in rv32ui rv32um rv32ua rv32si rv32mi; do
	for src in "$T/isa/$suite"/*.S; do
		t=$(basename "$src" .S)
		[[ -n "$FILTER" && "$suite-$t" != *$FILTER* ]] && continue
		build_p "$suite" "$t" && run "$suite-p-$t"
		# The v environment only supports user-level suites.
		if [[ $suite == rv32u* ]]; then
			build_v "$suite" "$t" && run "$suite-v-$t"
		fi
	done
done

echo "riscv-tests: $pass passed, $fail failed, $skipped did not build"
for f in "${failed[@]}"; do echo "  FAIL $f"; done
[ "$fail" -eq 0 ]
