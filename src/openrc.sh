# This file is part of apk-autoupdate package and is licensed under MIT license.

# Prints name of the service that started process with the specified PID and
# returns 0, or returns non-zero if not found.
# $1: PID
find_service_by_pid() {
	local pid="$1"

	edebug "Executing: rc-service-pid $pid"
	rc-service-pid "$pid"
}

# Controls OpenRC service.
# $1: service name
# $2+: options to pass into the init script
service_ctl() {
	local svcname="$1"; shift

	edebug "Executing: /etc/init.d/$svcname $rc_service_opts $*"
	# Note: --dry-run actually does not work!
	# Reported to upstream: https://github.com/OpenRC/openrc/issues/224.
	[ $DRY_RUN ] || /etc/init.d/$svcname $rc_service_opts "$@"
}
