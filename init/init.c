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

int main(void) {
	setup_filesystems();
	redirect_to_console();

	setenv("DPREFIX", "/var/cache/darling/prefix", 1);
	setenv("HOME", "/var/cache/darling", 1);
	setenv("PATH", "/usr/local/bin:/usr/bin:/bin:/sbin:/usr/sbin", 1);
	setenv("TERM", "linux", 1);

	// Default sigmask to allow normal signal handling. PID 1 ignores
	// most signals by default; we want SIGCHLD delivered.
	signal(SIGCHLD, SIG_DFL);

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
