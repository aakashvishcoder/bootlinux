CXX      ?= g++
CXXFLAGS ?= -O2 -g -std=c++17 -Wall -Wextra -Wno-unused-parameter
SRCS     := src/main.cpp src/cpu.cpp src/sbi.cpp src/devices.cpp
HDRS     := $(wildcard src/*.h)

all: build/bootlinux build/bootlinux.dtb

build/bootlinux: $(SRCS) $(HDRS)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ $(SRCS)

build/bootlinux.dtb: dts/bootlinux.dts
	@mkdir -p build
	dtc -I dts -O dtb -o $@ $<

# Build the kernel + initramfs (needs LINUX_SRC=/path/to/linux-6.12).
kernel:
	linux/build.sh

boot: all
	build/bootlinux build/Image

demo: all
	build/bootlinux --script tests/demo.txt build/Image

test: all
	tests/run_riscv_tests.sh

study: all
	python3 tools/tlb_study.py

clean:
	rm -f build/bootlinux build/bootlinux.dtb

.PHONY: all kernel boot demo test study clean
