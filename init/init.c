// SPDX-License-Identifier: BSD-2-Clause
//
// xnulinux init: minimal PID 1 that mounts the bare filesystems Darling
// needs and execs `darling shell` on the system console. Reaps zombies
// (PID 1's job). Restarts darling if it exits.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void say(const char *msg) {
	(void)write(2, msg, strlen(msg));
}

static void try_mount(const char *src, const char *tgt, const char *fs,
		      unsigned long flags, const char *data) {
	if (mkdir(tgt, 0755) != 0 && errno != EEXIST) {
		say("mkdir failed: ");
		say(tgt);
		say("\n");
	}
	if (mount(src, tgt, fs, flags, data) != 0 && errno != EBUSY) {
		say("mount failed: ");
		say(tgt);
		say("\n");
	}
}

static void setup_filesystems(void) {
	try_mount("proc", "/proc", "proc", 0, NULL);
	try_mount("sys", "/sys", "sysfs", 0, NULL);
	try_mount("dev", "/dev", "devtmpfs", 0, NULL);
	// devpts for pseudo-terminals (ttys), shm for POSIX shared memory —
	// systemd normally sets these up; we have to ourselves.
	try_mount("devpts", "/dev/pts", "devpts", 0, "gid=5,mode=620");
	try_mount("shm", "/dev/shm", "tmpfs", 0, "mode=1777");
	try_mount("run", "/run", "tmpfs", 0, NULL);
	try_mount("tmp", "/tmp", "tmpfs", 0, NULL);
	// Darling's overlayfs workdir is sibling of DPREFIX, so mount tmpfs
	// at the parent and put the prefix inside it (same filesystem rule).
	try_mount("darling-prefix", "/var/cache/darling", "tmpfs", 0, NULL);
}

static void redirect_to_console(void) {
	int fd = open("/dev/console", O_RDWR);
	if (fd < 0)
		return;
	(void)dup2(fd, 0);
	(void)dup2(fd, 1);
	(void)dup2(fd, 2);
	if (fd > 2)
		(void)close(fd);
}

// Fork+exec a daemon-style helper. Returns immediately; the child
// runs and (typically) daemonizes itself.
static void spawn_detached(const char *path, char *const argv[]) {
	pid_t pid = fork();
	if (pid < 0) {
		say("fork failed for ");
		say(path);
		say("\n");
		return;
	}
	if (pid == 0) {
		execv(path, argv);
		say("exec failed: ");
		say(path);
		say("\n");
		_exit(127);
	}
}

// Bring up loopback and run dhclient on every non-loopback interface.
// dhclient daemonizes by default, so init doesn't have to babysit it.
// TODO: migrate to Darwin's configd + ipconfig once they translate
// network ioctls correctly.
static void start_networking(void) {
	char *lo_args[] = {"ip", "link", "set", "lo", "up", NULL};
	pid_t lo = fork();
	if (lo == 0) {
		execv("/usr/sbin/ip", lo_args);
		_exit(127);
	}
	if (lo > 0) waitpid(lo, NULL, 0);

	char *dh_args[] = {"dhclient", "-1", NULL};
	spawn_detached("/sbin/dhclient", dh_args);
}

// Generate fresh host keys (the squashfs ships with them removed so
// every boot gets unique keys) and start sshd. sshd daemonizes by
// default. TODO: switch to Darwin's openssh in the prefix once the
// networking syscall translation supports it.
static void start_sshd(void) {
	char *keygen_args[] = {"ssh-keygen", "-A", NULL};
	pid_t k = fork();
	if (k == 0) {
		execv("/usr/bin/ssh-keygen", keygen_args);
		_exit(127);
	}
	if (k > 0) waitpid(k, NULL, 0);

	if (mkdir("/run/sshd", 0755) != 0 && errno != EEXIST) {
		say("mkdir /run/sshd failed\n");
	}

	char *sshd_args[] = {"sshd", NULL};
	spawn_detached("/usr/sbin/sshd", sshd_args);
}

int main(void) {
	setup_filesystems();
	redirect_to_console();

	setenv("DPREFIX", "/var/cache/darling/prefix", 1);
	setenv("HOME", "/var/cache/darling", 1);
	setenv("PATH", "/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin", 1);
	setenv("TERM", "linux", 1);

	// Console colors (blues/purples from BSD ls, etc.) are unreadable
	// on the serial console. NO_COLOR is the de-facto standard; CLICOLOR=0
	// kills BSD ls; clearing LS_COLORS handles GNU-coreutils cases.
	setenv("NO_COLOR", "1", 1);
	setenv("CLICOLOR", "0", 1);
	unsetenv("LS_COLORS");
	unsetenv("LSCOLORS");

	// Default sigmask to allow normal signal handling. PID 1 ignores
	// most signals by default; we want SIGCHLD delivered.
	signal(SIGCHLD, SIG_DFL);

	start_networking();
	start_sshd();

	for (;;) {
		pid_t pid = fork();
		if (pid < 0) {
			say("fork failed\n");
			sleep(1);
			continue;
		}

		if (pid == 0) {
			// Child: exec darling shell.
			execl("/usr/local/bin/darling", "darling", "shell",
			      (char *)NULL);
			say("exec /usr/local/bin/darling failed\n");
			_exit(127);
		}

		// Parent (PID 1): reap any zombies until our darling child exits.
		for (;;) {
			int status;
			pid_t w = waitpid(-1, &status, 0);
			if (w == -1) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (w == pid)
				break;
		}

		say("\ndarling exited; restarting in 1s...\n");
		sleep(1);
	}
}
