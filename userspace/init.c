/*
 * bootlinux init: a tiny shell that runs as PID 1.
 *
 * Built against the kernel's own nolibc (tools/include/nolibc), so it needs
 * no C library at all: every libc call below is a thin wrapper around a raw
 * Linux system call. That keeps the whole user space to one static binary.
 */
#include "nolibc.h"

#define LINE_MAX 256
#define ARGS_MAX 16

static void puts_fd(int fd, const char *s) { write(fd, s, strlen(s)); }
static void out(const char *s) { puts_fd(1, s); }
static void err(const char *s) { puts_fd(2, s); }

static void out_num(unsigned long v)
{
	char buf[16];
	int i = sizeof(buf);
	buf[--i] = 0;
	do { buf[--i] = '0' + v % 10; v /= 10; } while (v);
	out(buf + i);
}

/* Read one line from stdin, echoing nothing (the tty does that). */
static int read_line(char *buf, int max)
{
	int n = 0;
	while (n < max - 1) {
		char c;
		ssize_t r = read(0, &c, 1);
		if (r <= 0)
			return n ? n : -1;
		if (c == '\n' || c == '\r')
			break;
		buf[n++] = c;
	}
	buf[n] = 0;
	return n;
}

static int split(char *line, char **argv)
{
	int argc = 0;
	while (*line && argc < ARGS_MAX - 1) {
		while (*line == ' ' || *line == '\t')
			*line++ = 0;
		if (!*line)
			break;
		argv[argc++] = line;
		while (*line && *line != ' ' && *line != '\t')
			line++;
	}
	argv[argc] = 0;
	return argc;
}

static void cmd_cat(int argc, char **argv)
{
	char buf[256];
	for (int i = 1; i < argc; i++) {
		int fd = open(argv[i], O_RDONLY, 0);
		if (fd < 0) { err("cat: cannot open "); err(argv[i]); err("\n"); continue; }
		ssize_t n;
		while ((n = read(fd, buf, sizeof(buf))) > 0)
			write(1, buf, n);
		close(fd);
	}
}

struct dirent64_raw {
	uint64_t d_ino;
	int64_t d_off;
	unsigned short d_reclen;
	unsigned char d_type;
	char d_name[];
};

static void cmd_ls(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : ".";
	int fd = open(path, O_RDONLY | O_DIRECTORY, 0);
	if (fd < 0) { err("ls: cannot open "); err(path); err("\n"); return; }
	char buf[1024];
	long n;
	while ((n = my_syscall3(__NR_getdents64, fd, buf, sizeof(buf))) > 0) {
		for (long off = 0; off < n;) {
			struct dirent64_raw *d = (struct dirent64_raw *)(buf + off);
			if (d->d_name[0] != '.') {
				out(d->d_name);
				out(d->d_type == 4 ? "/  " : "  ");
			}
			off += d->d_reclen;
		}
	}
	out("\n");
	close(fd);
}

static void cmd_uname(void)
{
	struct utsname u;
	if (uname(&u) == 0) {
		out(u.sysname); out(" "); out(u.nodename); out(" ");
		out(u.release); out(" "); out(u.machine); out("\n");
	}
}

/*
 * memtouch PAGES [ROUNDS]: map PAGES pages and write one word in each,
 * ROUNDS times. Every access lands on a different page, so this is a
 * deliberately TLB-hostile workload for the emulator's TLB study.
 */
static void cmd_memtouch(int argc, char **argv)
{
	unsigned long pages = argc > 1 ? atoi(argv[1]) : 256;
	unsigned long rounds = argc > 2 ? atoi(argv[2]) : 4;
	unsigned long len = pages * 4096;
	volatile unsigned int *p = mmap(0, len, PROT_READ | PROT_WRITE,
					MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) { err("memtouch: mmap failed\n"); return; }
	unsigned int sum = 0;
	for (unsigned long r = 0; r < rounds; r++)
		for (unsigned long i = 0; i < pages; i++) {
			p[i * 1024] += i + r;
			sum += p[i * 1024];
		}
	munmap((void *)p, len);
	out("memtouch: "); out_num(pages); out(" pages x "); out_num(rounds);
	out(" rounds, checksum "); out_num(sum); out("\n");
}

/* primes N: a compute-bound workload that stays on a handful of pages. */
static void cmd_primes(int argc, char **argv)
{
	unsigned long n = argc > 1 ? atoi(argv[1]) : 10000, count = 0;
	for (unsigned long i = 2; i <= n; i++) {
		int prime = 1;
		for (unsigned long d = 2; d * d <= i; d++)
			if (i % d == 0) { prime = 0; break; }
		count += prime;
	}
	out("primes up to "); out_num(n); out(": "); out_num(count); out("\n");
}

static void help(void)
{
	out("commands: help echo cat ls cd pwd uname mkdir\n"
	    "          primes [N]  memtouch [PAGES] [ROUNDS]  poweroff\n");
}

static void run(int argc, char **argv)
{
	const char *c = argv[0];
	if (!strcmp(c, "help")) help();
	else if (!strcmp(c, "echo")) {
		for (int i = 1; i < argc; i++) { out(argv[i]); out(i + 1 < argc ? " " : ""); }
		out("\n");
	}
	else if (!strcmp(c, "cat")) cmd_cat(argc, argv);
	else if (!strcmp(c, "ls")) cmd_ls(argc, argv);
	else if (!strcmp(c, "cd")) { if (chdir(argc > 1 ? argv[1] : "/")) err("cd: no such directory\n"); }
	else if (!strcmp(c, "pwd")) { char b[128]; if (my_syscall2(__NR_getcwd, b, sizeof(b)) > 0) { out(b); out("\n"); } }
	else if (!strcmp(c, "uname")) cmd_uname();
	else if (!strcmp(c, "mkdir")) { if (argc < 2 || mkdir(argv[1], 0755)) err("mkdir: failed\n"); }
	else if (!strcmp(c, "primes")) cmd_primes(argc, argv);
	else if (!strcmp(c, "memtouch")) cmd_memtouch(argc, argv);
	else if (!strcmp(c, "poweroff")) {
		out("powering off\n");
		my_syscall0(__NR_sync);
		reboot(LINUX_REBOOT_CMD_POWER_OFF);
	}
	else { err(c); err(": unknown command (try help)\n"); }
}

int main(void)
{
	mkdir("/proc", 0555);
	mkdir("/sys", 0555);
	mount("proc", "/proc", "proc", 0, 0);
	mount("sysfs", "/sys", "sysfs", 0, 0);

	out("\nbootlinux shell - type 'help'\n");
	char line[LINE_MAX];
	char *argv[ARGS_MAX];
	for (;;) {
		out("# ");
		int n = read_line(line, sizeof(line));
		if (n < 0) {
			/* stdin closed (scripted run finished): shut down cleanly */
			reboot(LINUX_REBOOT_CMD_POWER_OFF);
			for (;;) sched_yield();
		}
		int argc = split(line, argv);
		if (argc)
			run(argc, argv);
	}
}
