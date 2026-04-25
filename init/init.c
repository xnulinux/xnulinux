// SPDX-License-Identifier: BSD-2-Clause
//
// xnulinux init: minimal PID 1 that mounts the bare filesystems Darling
// needs, configures sshd, hands sshd to launchd, and execs an interactive
// `darling shell` on /dev/console. Reaps zombies (PID 1's job) and
// restarts darling if it exits.

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

// One-time per boot: drop a setup script into /tmp and run it inside the
// Darling prefix. The script edits sshd_config, installs root's
// authorized_keys, and hands sshd to launchd in inetd-on-demand mode on
// port 22. After this returns, sshd is supervised by launchd and accepts
// connections without any further help from init.c.
static void setup_sshd_via_launchd(void) {
	static const char setup_script[] =
		"#!/bin/bash\n"
		"set -e\n"
		// sshd uses the FIRST value seen for each keyword, so prepend
		// our overrides rather than appending.
		"{ echo 'PermitRootLogin prohibit-password'; "
		"echo 'PubkeyAuthentication yes'; "
		"echo 'PasswordAuthentication no'; "
		"echo 'ChallengeResponseAuthentication no'; "
		"echo 'KbdInteractiveAuthentication no'; "
		"cat /etc/ssh/sshd_config; } > /tmp/sshd_config.new && "
		"mv /tmp/sshd_config.new /etc/ssh/sshd_config\n"
		// Apple/BSD sed: -i needs an explicit backup extension.
		"sed -i '' 's/^UsePAM yes/UsePAM no/' /etc/ssh/sshd_config\n"
		// Apple's crypt() is DES-only and can't match the $6$ hash
		// that setupPrefix bakes into master.passwd, so password auth
		// is unusable. Pubkey only.
		"mkdir -p /var/root/.ssh\n"
		"chmod 700 /var/root/.ssh\n"
		"cat > /var/root/.ssh/authorized_keys <<'EOF'\n"
		"ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGVGr9MD8yEx1OFqkh8tu+KytxVrcMA1V2lXkycz0tGh jmaloney@Josephs-Mini.home.local\n"
		"EOF\n"
		"chmod 600 /var/root/.ssh/authorized_keys\n"
		// -w writes Disabled=false to launchd's overrides.plist (the
		// dir was pre-created by setupPrefix). sshd-keygen-wrapper
		// generates host keys lazily on the first connection.
		"launchctl load -w /System/Library/LaunchDaemons/ssh.plist\n";

	const char *script_path = "/tmp/xnulinux-sshd-setup.sh";
	FILE *f = fopen(script_path, "w");
	if (!f) {
		say("failed to write sshd setup script\n");
		return;
	}
	fputs(setup_script, f);
	fclose(f);
	chmod(script_path, 0755);

	pid_t pid = fork();
	if (pid < 0) {
		say("fork failed for sshd setup\n");
		return;
	}
	if (pid == 0) {
		// `darling shell` quote-wraps extra args before bash -c, which
		// would collapse a multi-line C string into one quoted word.
		// Pass the script's path (visible inside the prefix at
		// /Volumes/SystemRoot/tmp/...) and let bash exec it.
		execl("/usr/local/bin/darling", "darling", "shell",
		      "/Volumes/SystemRoot/tmp/xnulinux-sshd-setup.sh",
		      (char *)NULL);
		_exit(127);
	}

	int status;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		continue;
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

	// First darling-shell invocation: bring up the prefix, configure
	// sshd, hand it to launchd. Runs once and returns.
	setup_sshd_via_launchd();

	// Second darling-shell invocation: foreground interactive bash on
	// /dev/console for the operator. Restart-on-exit so an accidental
	// ^D doesn't leave the VM with no console to type at.
	for (;;) {
		pid_t pid = fork();
		if (pid < 0) {
			say("fork failed\n");
			sleep(1);
			continue;
		}

		if (pid == 0) {
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
