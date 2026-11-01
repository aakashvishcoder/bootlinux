#!/usr/bin/env bash
# Build the init shell and an RV32 Linux kernel with it as the initramfs.
#   LINUX_SRC=/path/to/linux-6.12 linux/build.sh            -> build/Image
#   LINUX_SRC=... linux/build.sh megapages                  -> build/Image-megapages
#
# The "megapages" variant applies linux/rv32-linear-megapages.patch, which
# lets RV32 map the kernel's linear map (all of RAM) with 4 MiB pages
# instead of 4 KiB ones. Stock 6.12 only allows that on 64-bit.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
LINUX_SRC=${LINUX_SRC:?set LINUX_SRC to a Linux 6.12 source tree}
CROSS=${CROSS:-riscv64-linux-gnu-}
VARIANT=${1:-stock}
case $VARIANT in
	stock) OUT=$ROOT/build/linux; IMAGE=$ROOT/build/Image ;;
	megapages) OUT=$ROOT/build/linux-megapages; IMAGE=$ROOT/build/Image-megapages ;;
	*) echo "unknown variant $VARIANT (stock|megapages)" >&2; exit 2 ;;
esac
mkdir -p "$OUT" "$ROOT/build"

# 1. User space: one static binary, no libc (kernel's nolibc header).
"${CROSS}gcc" -march=rv32ima_zicsr_zifencei -mabi=ilp32 -Os -static -nostdlib \
	-fno-stack-protector -fno-asynchronous-unwind-tables -Wall \
	-I "$LINUX_SRC/tools/include/nolibc" -include nolibc.h \
	-o "$ROOT/build/init" "$ROOT/userspace/init.c"

# 2. Root filesystem description, with absolute paths filled in.
sed "s|BOOTLINUX_ROOT|$ROOT|" "$ROOT/linux/initramfs.list" > "$OUT/initramfs.list"

# 3. Kernel patch for the variant, reverted again when the script exits.
if [ "$VARIANT" = megapages ]; then
	patch -d "$LINUX_SRC" -p1 --forward -s < "$ROOT/linux/rv32-linear-megapages.patch"
	trap 'patch -d "$LINUX_SRC" -p1 -R -s < "$ROOT/linux/rv32-linear-megapages.patch"' EXIT
fi

# 4. Kernel: tinyconfig + our fragment.
K="make -C $LINUX_SRC O=$OUT ARCH=riscv CROSS_COMPILE=$CROSS -j$(nproc)"
$K tinyconfig >/dev/null
"$LINUX_SRC/scripts/kconfig/merge_config.sh" -m -O "$OUT" "$OUT/.config" \
	"$ROOT/linux/bootlinux.config" >/dev/null
$K olddefconfig >/dev/null
$K Image
cp "$OUT/arch/riscv/boot/Image" "$IMAGE"
echo "kernel: $IMAGE ($(stat -c %s "$IMAGE") bytes)"
