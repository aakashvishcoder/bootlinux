#!/usr/bin/env bash
# Build the init shell and an RV32 Linux kernel with it as the initramfs.
#   LINUX_SRC=/path/to/linux-6.12 linux/build.sh
# Output: build/Image (flat kernel binary the emulator loads at 0x80000000)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LINUX_SRC=${LINUX_SRC:?set LINUX_SRC to a Linux 6.12 source tree}
CROSS=${CROSS:-riscv64-linux-gnu-}
OUT=$ROOT/build/linux
mkdir -p "$OUT" "$ROOT/build"

# 1. User space: one static binary, no libc (kernel's nolibc header).
"${CROSS}gcc" -march=rv32ima_zicsr_zifencei -mabi=ilp32 -Os -static -nostdlib \
	-fno-stack-protector -fno-asynchronous-unwind-tables -Wall \
	-I "$LINUX_SRC/tools/include/nolibc" -include nolibc.h \
	-o "$ROOT/build/init" "$ROOT/userspace/init.c"

# 2. Root filesystem description, with absolute paths filled in.
sed "s|BOOTLINUX_ROOT|$ROOT|" "$ROOT/linux/initramfs.list" > "$OUT/initramfs.list"

# 3. Kernel: tinyconfig + our fragment.
K="make -C $LINUX_SRC O=$OUT ARCH=riscv CROSS_COMPILE=$CROSS -j$(nproc)"
$K tinyconfig >/dev/null
"$LINUX_SRC/scripts/kconfig/merge_config.sh" -m -O "$OUT" "$OUT/.config" \
	"$ROOT/linux/bootlinux.config" >/dev/null
$K olddefconfig >/dev/null
$K Image
cp "$OUT/arch/riscv/boot/Image" "$ROOT/build/Image"
echo "kernel: $ROOT/build/Image ($(stat -c %s "$ROOT/build/Image") bytes)"
