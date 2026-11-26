/*
 * ruby_ctld: lightweight TCP control/debug daemon on :2323 for HakiOS Darwin (ruby).
 * Speaks the same prompt ("hakios> ") as v0.55f/v0.56b so tools/ruby_net.py works directly.
 *
 * Commands:
 *   help                 list commands
 *   klog                 dump kernel log (sysctl kern.hakios.klog)
 *   nbd                  dump NBD status (sysctl kern.hakios.nbd)
 *   sysctl <name>        read a string/int sysctl
 *   touch <args...>      run /redwood/bin/rwtouch <args...> (tap, swipe, button, notify, ...)
 *   run <prog> [args...] exec a program and stream stdout+stderr
 *   ls [dir]             list directory contents
 *   cat <file>           print file contents (up to 64 KiB)
 *   write <file> <text>  write a line to a file (for persistence tests)
 *   sync                 call sync() to flush HFS+/NBD buffers
 *   quit / exit          close connection
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>

static void
write_all(int fd, const void *buf, size_t len)
{
	const char *p = (const char *)buf;
	while (len > 0) {
		ssize_t n = write(fd, p, len);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) continue;
			return;
		}
		p += n;
		len -= (size_t)n;
	}
}

static void
send_str(int fd, const char *s)
{
	write_all(fd, s, strlen(s));
}

static void
cmd_sysctl(int fd, const char *name)
{
	size_t len = 0;
	if (sysctlbyname(name, NULL, &len, NULL, 0) != 0) {
		char msg[128];
		snprintf(msg, sizeof(msg), "sysctl %s: %s\n", name, strerror(errno));
		send_str(fd, msg);
		return;
	}
	if (len == 0) {
		send_str(fd, "(empty)\n");
		return;
	}
	if (len > 256 * 1024) len = 256 * 1024;
	char *buf = malloc(len + 1);
	if (!buf) {
		send_str(fd, "oom\n");
		return;
	}
	if (sysctlbyname(name, buf, &len, NULL, 0) == 0) {
		if (len == 4 || len == 8) {
			/* Could be binary int or short string; check printability */
			int printable = 1;
			size_t slen = len;
			if (slen > 0 && buf[slen - 1] == '\0') slen--;
			for (size_t i = 0; i < slen; i++) {
				unsigned char c = (unsigned char)buf[i];
				if (c < 0x09 || (c > 0x0d && c < 0x20) || c > 0x7e) {
					printable = 0;
					break;
				}
			}
			if (!printable) {
				char num[64];
				if (len == 4) {
					int v = 0;
					memcpy(&v, buf, 4);
					snprintf(num, sizeof(num), "%d (0x%x)\n", v, (unsigned)v);
				} else {
					long long v = 0;
					memcpy(&v, buf, 8);
					snprintf(num, sizeof(num), "%lld (0x%llx)\n", v, (unsigned long long)v);
				}
				send_str(fd, num);
				free(buf);
				return;
			}
		}
		size_t out_len = len;
		while (out_len > 0 && buf[out_len - 1] == '\0') out_len--;
		write_all(fd, buf, out_len);
		if (out_len == 0 || buf[out_len - 1] != '\n') send_str(fd, "\n");
	} else {
		char msg[128];
		snprintf(msg, sizeof(msg), "sysctl %s: %s\n", name, strerror(errno));
		send_str(fd, msg);
	}
	free(buf);
}

static void
cmd_exec(int fd, char *argv[])
{
	int pfd[2];
	if (pipe(pfd) != 0) {
		send_str(fd, "pipe failed\n");
		return;
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		send_str(fd, "fork failed\n");
		return;
	}
	if (pid == 0) {
		close(pfd[0]);
		dup2(pfd[1], 1);
		dup2(pfd[1], 2);
		close(pfd[1]);
		int dn = open("/dev/null", O_RDONLY);
		if (dn >= 0) {
			dup2(dn, 0);
			if (dn > 2) close(dn);
		}
		execv(argv[0], argv);
		dprintf(2, "exec %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	close(pfd[1]);
	char buf[2048];
	ssize_t n;
	while ((n = read(pfd[0], buf, sizeof(buf))) > 0) {
		write_all(fd, buf, (size_t)n);
	}
	close(pfd[0]);
	int st = 0;
	waitpid(pid, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) != 0) {
		char msg[64];
		snprintf(msg, sizeof(msg), "[exit %d]\n", WEXITSTATUS(st));
		send_str(fd, msg);
	} else if (WIFSIGNALED(st)) {
		char msg[64];
		snprintf(msg, sizeof(msg), "[sig %d]\n", WTERMSIG(st));
		send_str(fd, msg);
	}
}

static void
cmd_ls(int fd, const char *path)
{
	DIR *d = opendir(path);
	if (!d) {
		char msg[256];
		snprintf(msg, sizeof(msg), "ls %s: %s\n", path, strerror(errno));
		send_str(fd, msg);
		return;
	}
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
		char full[512];
		snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
		struct stat st;
		char line[600];
		if (lstat(full, &st) == 0) {
			char t = S_ISDIR(st.st_mode) ? 'd' : S_ISLNK(st.st_mode) ? 'l' : '-';
			snprintf(line, sizeof(line), "%c %9lld %s\n", t, (long long)st.st_size, de->d_name);
		} else {
			snprintf(line, sizeof(line), "?         ? %s\n", de->d_name);
		}
		send_str(fd, line);
	}
	closedir(d);
}

static void
cmd_cat(int fd, const char *path)
{
	int f = open(path, O_RDONLY);
	if (f < 0) {
		char msg[256];
		snprintf(msg, sizeof(msg), "cat %s: %s\n", path, strerror(errno));
		send_str(fd, msg);
		return;
	}
	char buf[4096];
	size_t total = 0;
	ssize_t n;
	while (total < 65536 && (n = read(f, buf, sizeof(buf))) > 0) {
		write_all(fd, buf, (size_t)n);
		total += (size_t)n;
	}
	close(f);
}

static void
handle_client(int fd)
{
	send_str(fd, "HakiOS ruby_ctld (Darwin 22.5 / iOS 16.5)\n");
	send_str(fd, "Type 'help' for commands.\n");
	char line[1024];
	for (;;) {
		send_str(fd, "hakios> ");
		size_t pos = 0;
		for (;;) {
			char c;
			ssize_t r = read(fd, &c, 1);
			if (r <= 0) return;
			if (c == '\r') continue;
			if (c == '\n') break;
			if (pos + 1 < sizeof(line)) line[pos++] = c;
		}
		line[pos] = '\0';
		/* trim leading/trailing spaces */
		char *s = line;
		while (*s == ' ' || *s == '\t') s++;
		size_t sl = strlen(s);
		while (sl > 0 && (s[sl - 1] == ' ' || s[sl - 1] == '\t')) s[--sl] = '\0';
		if (*s == '\0') continue;

		if (strcmp(s, "quit") == 0 || strcmp(s, "exit") == 0) {
			send_str(fd, "bye\n");
			return;
		} else if (strcmp(s, "help") == 0) {
			send_str(fd,
			    "Commands:\n"
			    "  klog                 kernel ring buffer (mtk_touch, nbd, usb)\n"
			    "  nbd                  NBD units status\n"
			    "  sysctl <name>        read sysctl\n"
			    "  touch <args...>      /redwood/bin/rwtouch <args...> (tap/swipe/button/notify)\n"
			    "  run <prog> [args...] execute binary and return output\n"
			    "  ls [path]            list directory\n"
			    "  cat <path>           read file\n"
			    "  write <path> <text>  write text line to file\n"
			    "  sync                 flush filesystems\n"
			    "  quit                 disconnect\n");
		} else if (strcmp(s, "klog") == 0) {
			cmd_sysctl(fd, "kern.hakios.klog");
		} else if (strcmp(s, "nbd") == 0) {
			cmd_sysctl(fd, "kern.hakios.nbd");
		} else if (strcmp(s, "sync") == 0) {
			sync();
			send_str(fd, "sync ok\n");
		} else if (strncmp(s, "sysctl ", 7) == 0) {
			cmd_sysctl(fd, s + 7);
		} else if (strcmp(s, "ls") == 0) {
			cmd_ls(fd, "/");
		} else if (strncmp(s, "ls ", 3) == 0) {
			cmd_ls(fd, s + 3);
		} else if (strncmp(s, "cat ", 4) == 0) {
			cmd_cat(fd, s + 4);
		} else if (strncmp(s, "write ", 6) == 0) {
			char *p = s + 6;
			char *sp = strchr(p, ' ');
			if (!sp) {
				send_str(fd, "usage: write <path> <text>\n");
			} else {
				*sp++ = '\0';
				int f = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
				if (f < 0) {
					char msg[256];
					snprintf(msg, sizeof(msg), "write %s: %s\n", p, strerror(errno));
					send_str(fd, msg);
				} else {
					write_all(f, sp, strlen(sp));
					write_all(f, "\n", 1);
					fsync(f);
					close(f);
					sync();
					send_str(fd, "write ok\n");
				}
			}
		} else if (strncmp(s, "touch ", 6) == 0 || strncmp(s, "run ", 4) == 0) {
			char *argv[64];
			int argc = 0;
			char *tok;
			if (strncmp(s, "touch ", 6) == 0) {
				argv[argc++] = "/redwood/bin/rwtouch";
				tok = s + 6;
			} else {
				tok = s + 4;
			}
			while (*tok && argc < 63) {
				while (*tok == ' ' || *tok == '\t') tok++;
				if (!*tok) break;
				argv[argc++] = tok;
				while (*tok && *tok != ' ' && *tok != '\t') tok++;
				if (*tok) *tok++ = '\0';
			}
			argv[argc] = NULL;
			if (argc == 0) {
				send_str(fd, "missing program\n");
			} else {
				cmd_exec(fd, argv);
			}
		} else {
			send_str(fd, "unknown command (try 'help')\n");
		}
	}
}

int
main(void)
{
	signal(SIGPIPE, SIG_IGN);
	int srv = socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0) return 1;
	int one = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(2323);
	sa.sin_addr.s_addr = htonl(INADDR_ANY);

	for (int attempt = 0; attempt < 30; attempt++) {
		if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) == 0) break;
		sleep(1);
	}
	if (listen(srv, 4) != 0) return 2;

	for (;;) {
		int c = accept(srv, NULL, NULL);
		if (c < 0) {
			if (errno == EINTR) continue;
			sleep(1);
			continue;
		}
		handle_client(c);
		close(c);
	}
}
