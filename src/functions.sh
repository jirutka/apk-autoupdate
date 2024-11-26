# This file is part of apk-autoupdate package and is licensed under MIT license.

# Logs the message from STDIN or $1.
# $1: message (defaults is to read from STDIN)
emsg() {
	if [ $# -eq 0 ]; then
		awk "{ print \"$PROGNAME\", \$0; fflush(); }"
	else
		printf '%s: %s\n' "$PROGNAME" "$1"
	fi
}

# Logs the message from STDIN or $1 with level DEBUG.
# $1: message (defaults is to read from STDIN)
edebug() {
	if [ "$DEBUG" ]; then
		emsg "debug: $@" >&2
	elif [ $# -eq 0 ]; then
		cat >/dev/null
	fi
}

# Logs the message from STDIN or $1 with level INFO.
# $1: message (defaults is to read from STDIN)
einfo() {
	emsg "$@"
}

# Logs the message from STDIN on $1 with level WARN.
# $1: message (defaults is to read from STDIN)
ewarn() {
	emsg "$@" >&2
}

# Logs the error message and exits.
# $1: error message
# $2: exit status (default: 1)
die() {
	emsg "$1"
	exit ${2:-1}
}

# Returns 0 if "case" pattern $1 matches at least one of the following
# arguments, otherwise returns 1.
case_match() {
	local patt="${1%|}"; shift
	[ "$patt" ] || return 1

	local str; for str in "$@"; do
		eval "case \"$str\" in $patt) return 0;; *);; esac"
	done
	return 1
}

# Prints arguments that match the "case" pattern. Returns 0 if at least one
# match is found.
# $1: case pattern
# $@: elements to test
case_filter() {
	local patt="${1%|}"; shift
	local retval=1
	[ "$patt" ] || return 1

	local str; for str in "$@"; do
		if eval "case \"$str\" in $patt) true;; *) false;; esac"; then
			printf '%s\n' "$str"
			retval=0
		fi
	done
	return $retval
}

# Formats $@ for "case" pattern (including sanitization).
case_patt() {
	printf '%s\n' "$@" | sed -e 's/[();| ]/\\&/g' | tr '\n' '|'
}

# Returns 0 if item $1 is contained in list $@, otherwise returns 1.
list_has() {
	local needle="$1"; shift

	local i; for i in "$@"; do
		[ "$needle" = "$i" ] && return 0
	done
	return 1
}

# Prints command line of the specified process.
# $1: PID
proc_cmdline() {
	[ -r "/proc/$1/cmdline" ] || return 1
	cat "/proc/$1/cmdline" | xargs -0
}

# Prints executable of the specified process. Suffix " (deleted)" and
# ".apk-new" is stripped, if present.
# $1: PID
proc_exe() {
	local pid="$1"
	local path

	path=$(readlink "/proc/$pid/exe" 2>/dev/null) || return 1
	path="${path% (deleted)}"

	printf '%s\n' "${path%.apk-new}"
}

# Prints PIDs of processes that use (maps into memory) files which have been
# deleted or replaced (with different content) on disk.
# $1: patterns to exclude/include certain paths from checking
procs_using_modified_files() {
	local retval=0

	set -f  # disable globbing
	local opts=$(printf -- '-f %s ' ${1:-*})

	edebug "Executing: procs-need-restart $opts"
	procs-need-restart $opts || retval=$?

	set +f  # enable globbing
	return $retval
}
