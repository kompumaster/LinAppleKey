#!/bin/sh
# LinAppleKey uninstaller.
#
#   sudo ./uninstall.sh            keep /etc/linapplekey.conf
#   sudo ./uninstall.sh --purge    remove the config too
set -eu

PURGE=0

usage()
{
	cat <<'EOF'
Usage: sudo ./uninstall.sh [options]

Options:
  --purge    also remove /etc/linapplekey.conf
  -h, --help show this help
EOF
}

for arg in "$@"; do
	case "$arg" in
		--purge)   PURGE=1 ;;
		-h|--help) usage; exit 0 ;;
		*)         echo "unknown option: $arg" >&2; usage >&2; exit 2 ;;
	esac
done

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo ./uninstall.sh" >&2
	exit 1
fi

if command -v systemctl >/dev/null 2>&1; then
	systemctl disable --now linapplekeyd.service 2>/dev/null || true
fi

rm -f /etc/systemd/system/linapplekeyd.service

if command -v systemctl >/dev/null 2>&1; then
	systemctl daemon-reload 2>/dev/null || true
	systemctl reset-failed linapplekeyd.service 2>/dev/null || true
fi

rm -f /usr/local/bin/linapplekeyd
rm -rf /usr/local/share/doc/linapplekey

if [ "$PURGE" -eq 1 ]; then
	rm -f /etc/linapplekey.conf
	echo "removed /etc/linapplekey.conf"
else
	echo "kept /etc/linapplekey.conf (use --purge to remove it)"
fi

echo "LinAppleKey removed."
