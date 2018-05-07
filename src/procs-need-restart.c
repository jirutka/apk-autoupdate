/*
 * The MIT License
 *
 * Copyright 2018 Jakub Jirutka <jakub@jirutka.cz>.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PROCFS_PATH
#define PROCFS_PATH            "/proc"
#endif

#ifndef VERSION
#define VERSION                unknown
#endif

#define PROGNAME               "procs-need-restart"

#define PROC_EXE_PATH          PROCFS_PATH "/%u/exe"
#define PROC_MAPS_PATH         PROCFS_PATH "/%u/maps"
#define PROC_MAP_FILES_PATH    PROCFS_PATH "/%u/map_files/%lx-%lx"
#define PROC_ROOT_PATH         PROCFS_PATH "/%u/root/%s"

#define EXIT_WRONG_USAGE       100
#define RET_ERROR              -1

#define FLAG_VERBOSE           0x0001
#define FLAG_IGNORE_EACCES     0x0002

// Length of highest pid_t (int) value encoded as a decimal number.
#define PID_STR_MAX            10


#define STR_(x) #x
#define STR(x) STR_(x)

#define foreach(var, array, term_value, ...) \
	for (size_t i = 0; (array)[i] != term_value; i++) { \
		var = (array)[i]; \
		__VA_ARGS__; \
	}

#define log_err(format, ...) \
	fprintf(stderr, PROGNAME ": " format "\n", __VA_ARGS__)


static const char *HELP_MSG =
	"Usage: " PROGNAME " [options] [PID...]\n"
	"\n"
	"Find processes that use (maps into memory) files which have been deleted\n"
	"or replaced on disk (and the new files are not identical to the mapped ones).\n"
	"If no PID is specified, scan all processes.  But if user's effective UID is\n"
	"not 0 (i.e. not root), ignore processes we don't have permissions to examine.\n"
	"\n"
	"This program is part of apk-autoupdate.\n"
	"\n"
	"Options:\n"
	"  -f PATT*   Specify paths of mapped files to include/exclude from checking.\n"
	"             Syntax is identical with fnmatch(3) with no flags, but with\n"
	"             leading \"!\" for negative match (exclude). This option may be\n"
	"             repeated.\n"
	"\n"
	"  -v         Report all affected mapped files.\n"
	"\n"
	"  -h         Show this message and exit.\n"
	"\n"
	"  -V         Print program version and exit.\n"
	"\n"
	"Please report bugs at <https://github.com/jirutka/apk-autoupdate/issues>\n";

static unsigned int flags = 0;

// Struct for storing selected fields from /proc/<pid>/maps entries.
struct map_info {
	ptrdiff_t start;
	ptrdiff_t end;
	unsigned int dev_major;
	unsigned long inode;
	char filename[PATH_MAX + 8];  // we need +1 for \0, but use 8 for better align
};


__attribute__((format(printf, 3, 4)))
static void str_fmt (char *buf, size_t buf_size, const char *format, ...) {

	va_list ap; va_start(ap, format);
	int len = vsnprintf(buf, buf_size, format, ap);
	va_end(ap);

	assert(len > 0 && len < (int)buf_size);
}

static bool str_chomp (char *str, const char *suffix) {
	size_t str_len = strlen(str);
	size_t suffix_len = strlen(suffix);

	if (str_len < suffix_len) {
		return false;
	}
	// Return false if *str* does not end with *suffix*.
	if (memcmp(&str[str_len - suffix_len], suffix, suffix_len) != 0) {
		return false;
	}
	str[str_len - suffix_len] = '\0';  // truncate suffix

	return true;
}

static int str_to_uint (const char *str) {
	if (!str || !isdigit(str[0])) {
		return RET_ERROR;
	}
	char *end;
	errno = 0;
	unsigned long res = strtoul(str, &end, 10);

	if (errno != 0 || res > INT_MAX || *end == str[0] || *end != '\0') {
		return RET_ERROR;
	}
	return (int) res;
}

static bool fnmatch_any (const char **patterns, const char *string, int flags) {
	bool negated = false;

	foreach(const char *item, patterns, NULL, {
		negated = item[0] == '!';

		if (fnmatch(negated ? (item) + 1 : item, string, flags) == 0) {
			return true ^ negated;
		}
	})
	return false;
}

static int cmp_files (const char *fname1, const char *fname2) {
	int res = RET_ERROR;

	int fd1 = -1, fd2 = -1;
	char *addr1 = NULL, *addr2 = NULL;
	size_t size = 0;

	if ((fd1 = open(fname1, O_RDONLY)) < 0) {
		goto done;
	}
	if ((fd2 = open(fname2, O_RDONLY)) < 0) {
		goto done;
	}

	{
		struct stat sb1, sb2;
		if (fstat(fd1, &sb1) < 0 || fstat(fd2, &sb2) < 0) {
			goto done;
		}
		if (sb1.st_size != sb2.st_size) {
			res = 1;  // files are different
			goto done;
		}
		size = (size_t) sb1.st_size;
	}

	if ((addr1 = mmap(NULL, size, PROT_READ, MAP_SHARED, fd1, 0)) == MAP_FAILED) {
		log_err("%s: %s", fname1, strerror(errno));
		goto done;
	}
	if ((addr2 = mmap(NULL, size, PROT_READ, MAP_SHARED, fd2, 0)) == MAP_FAILED) {
		log_err("%s: %s", fname2, strerror(errno));
		goto done;
	}

	res = memcmp(addr1, addr2, size) == 0 ? 0 : 1;

done:
	if (addr1) (void) munmap(addr1, size);
	if (addr2) (void) munmap(addr2, size);
	if (fd1 > 0) (void) close(fd1);
	if (fd2 > 0) (void) close(fd2);

	return res;
}

static pid_t next_pid (DIR *proc_dir) {
	struct dirent *entry;
	pid_t pid;

	while ((entry = readdir(proc_dir))) {
		if ((pid = str_to_uint(entry->d_name)) != -1) {
			return pid;
		}
	}
	return -1;
}

static bool is_kernel_proc (pid_t pid) {
	char exe_path[sizeof(PROC_EXE_PATH) + PID_STR_MAX + 1];

	// /proc/<pid>/exe
	str_fmt(exe_path, sizeof(exe_path), PROC_EXE_PATH, pid);

	// See https://stackoverflow.com/a/12231039/2217862.
	if (readlink(exe_path, (char[]) { '\0' }, 1) < 0 && errno == ENOENT) {
		struct stat sb;
		return lstat(exe_path, &sb) == 0;
	}
	return false;
}

static int proc_exists (pid_t pid) {

	if (kill(pid, 0) == 0) {
		return 0;
	} else if (errno == ESRCH) {
		return 1;
	} else {
		return RET_ERROR;
	}
}

static int resolve_link (const char *pathname, char *buff, size_t buff_size) {

	errno = 0;
	ssize_t size = readlink(pathname, buff, buff_size - 1);
	if (size < 0 || (size_t)size >= buff_size) {
		return RET_ERROR;
	}
	buff[size] = '\0';  // readlink does not end string with \0!

	return 0;
}

static int proc_maps_replaced_files (pid_t pid, const char **file_patterns) {
	int res = 1;
	struct map_info map;
	char last_filename[PATH_MAX + 1] = { '\0' };

	FILE *maps_fp;
	{
		char maps_path[sizeof(PROC_MAPS_PATH) + PID_STR_MAX + 1];
		str_fmt(maps_path, sizeof(maps_path), PROC_MAPS_PATH, pid);

		if ((maps_fp = fopen(maps_path, "r")) == NULL) {
			int fopen_err = errno;

			if (fopen_err == EACCES && flags & FLAG_IGNORE_EACCES) {
				return 1;  //  no
			}
			// If process does not exist anymore, then it's not an error.
			if (proc_exists(pid) == 1) {
				return 1;  // no
			}
			log_err("%s: %s", maps_path, strerror(fopen_err));
			return RET_ERROR;
		}
	}

	size_t buf_size = PATH_MAX + 1;
	char *buf = malloc(buf_size);

	while (getline(&buf, &buf_size, maps_fp) != -1) {
		// Strip " (deleted)\n" from the path and skip if not applicable, i.e.
		// the file has not been deleted or replaced.
		if (!str_chomp(buf, " (deleted)\n")) {
			continue;
		}
		// Strip .apk-new from the path (special case for apk-tools).
		(void) str_chomp(buf, ".apk-new");

		// Parse the line and skip if it has wrong format.
		if (sscanf(buf, "%lx-%lx %*c%*c%*c%*c %*x %x:%*x %lu%*[ \t]%" STR(PATH_MAX) "[^\n]s",
		           &map.start, &map.end, &map.dev_major, &map.inode, map.filename) < 5) {
			continue;
		}
		// One filename is typically repeated three times in a row with
		// different perms in /proc/<pid>/maps, so skip them.
		if (strcmp(map.filename, last_filename) == 0) {
			continue;
		}
		strncpy(last_filename, map.filename, sizeof(last_filename));

		// Skip non-file entries.
		// Entries like /SYSV00000000, /drm, /i915 etc. have major 0.
		if (map.inode == 0 || map.dev_major == 0) {
			continue;
		}
		// Skip files excluded based on given patterns, if any.
		if (file_patterns[0] && !fnmatch_any(file_patterns, map.filename, 0)) {
			continue;
		}
		// Compare the file on disk with the mapped one and skip if
		// they are identical.
		str_fmt(buf, buf_size, PROC_MAP_FILES_PATH, pid, map.start, map.end);
		if (cmp_files(map.filename, buf) == 0) {
			continue;
		}

		res = 0;  // yes
		if (flags & FLAG_VERBOSE) {
			printf("%d\t%s\n", pid, map.filename);
		} else {
			printf("%d\n", pid);
			break;
		}
	}

	free(buf);
	fclose(maps_fp);

	return res;
}

static int proc_has_replaced_exe (pid_t pid, const char **file_patterns) {
	char exe_path[sizeof(PROC_EXE_PATH) + PID_STR_MAX + 1];
	char link_path[PATH_MAX];

	// /proc/<pid>/exe
	str_fmt(exe_path, sizeof(exe_path), PROC_EXE_PATH, pid);

	if (resolve_link(exe_path, link_path, sizeof(link_path)) < 0) {
		int link_err = errno;

		if (link_err == EACCES && flags & FLAG_IGNORE_EACCES) {
			return 1;  //  no
		}
		// If process does not exist anymore, then it's not an error.
		if (proc_exists(pid) == 1) {
			return 1;  // no
		}
		log_err("%s: %s", exe_path, strerror(link_err));
		return RET_ERROR;
	}
	// Strip " (deleted)" from the path and return if not applicable, i.e.
	// the executable has not been deleted or replaced.
	if (!str_chomp(link_path, " (deleted)")) {
		return 1;  // no
	}
	// Strip .apk-new from the path (special case for apk-tools).
	(void) str_chomp(link_path, ".apk-new");

	// Skip files excluded based on given patterns, if any.
	if (file_patterns[0] && !fnmatch_any(file_patterns, link_path, 0)) {
		return 1;  // no
	}

	{
		char file_path[PATH_MAX];

		int len = snprintf(file_path, sizeof(file_path), PROC_ROOT_PATH, pid, link_path);
		if (len <= 0 || (size_t)len >= sizeof(file_path)) {
			log_err("too long file path: " PROC_ROOT_PATH, pid, link_path);

		// Compare the file on disk with the mapped one, return 1 (no) if they
		// are identical.
		} else if (cmp_files(exe_path, file_path) == 0) {
			return 1;  // no
		}
	}

	if (flags & FLAG_VERBOSE) {
		printf("%d\t%s\n", pid, link_path);
	} else {
		printf("%d\n", pid);
	}
	return 0;  // yes
}

static int scan_proc (pid_t pid, const char **file_patterns) {

	int res1 = proc_has_replaced_exe(pid, file_patterns);
	if (res1 == RET_ERROR) {
		return RET_ERROR;
	} else if (res1 == 0 && !(flags & FLAG_VERBOSE)) {
		return 0;
	}

	int res2 = proc_maps_replaced_files(pid, file_patterns);
	return res1 * res2;
}

static int scan_procs (pid_t *pids, const char **file_patterns) {
	int status = EXIT_SUCCESS;

	foreach(pid_t pid, pids, -1, {
		if (scan_proc(pid, file_patterns) < 0) {
			status = EXIT_FAILURE;
		}
	})
	return status;
}

static int scan_all_procs (const char **file_patterns) {
	int status = EXIT_SUCCESS;

	DIR *dir = opendir(PROCFS_PATH);
	if (!dir) {
		log_err("%s: %s", PROCFS_PATH, strerror(errno));
		return EXIT_FAILURE;
	}

	pid_t pid = next_pid(dir);
	if (pid == -1) {
		log_err("%s", "no processes found!");
		return EXIT_FAILURE;
	}
	while ((pid = next_pid(dir)) != -1) {
		// Skip kernel processes/threads.
		if (is_kernel_proc(pid)) continue;

		if (scan_proc(pid, file_patterns) < 0) {
			status = EXIT_FAILURE;
		}
	}
	closedir(dir);

	return status;
}

int main (int argc, char **argv) {
	const char *file_patterns[argc + 1];
	file_patterns[0] = NULL;

	{
		int optch;
		int f_cnt = 0;

		opterr = 0;  // don't print implicit error message on unrecognized option
		while ((optch = getopt(argc, argv, "f:hVv")) != -1) {
			switch (optch) {
				case 'f':
					file_patterns[f_cnt++] = (char *)optarg;
					break;
				case 'v':
					flags |= FLAG_VERBOSE;
					break;
				case 'h':
					printf("%s", HELP_MSG);
					return EXIT_SUCCESS;
				case 'V':
					printf("%s %s\n", PROGNAME, STR(VERSION));
					return EXIT_SUCCESS;
				default:
					log_err("invalid option: -%c\n", optopt);
					fprintf(stderr, "%s", HELP_MSG);
					return EXIT_WRONG_USAGE;
			}
		}
		file_patterns[f_cnt] = NULL;  // mark end of the array
	}

	if (optind < argc) {
		pid_t pids[argc - optind + 1];
		pids[0] = -1;

		for (int i = optind, pid; i < argc; i++) {
			if ((pid = atoi(argv[i])) < 1) {
				log_err("invalid argument: %s", argv[i]);
				return EXIT_WRONG_USAGE;
			}
			pids[i - optind] = (pid_t) pid;
		}
		pids[argc - optind] = -1;  // mark end of the array

		return scan_procs(pids, file_patterns);

	} else {
		if (geteuid() != 0) {
			flags |= FLAG_IGNORE_EACCES;
		}
		return scan_all_procs(file_patterns);
	}
}
