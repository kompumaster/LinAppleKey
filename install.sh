#!/bin/sh
# LinAppleKey installer. Run from the source tree or a release archive:
#
#   sudo ./install.sh
#
# Options:
#   --force-config   overwrite /etc/linapplekey.conf
#   --no-start       install files but do not enable/start the service
#   -h, --help       show this help
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

FORCE_CONFIG=0
NO_START=0

usage()
{
	cat <<'EOF'
Usage: sudo ./install.sh [options]

Options:
  --force-config   overwrite an existing /etc/linapplekey.conf
  --no-start       install files but do not enable/start the service
  -h, --help       show this help
EOF
}

for arg in "$@"; do
	case "$arg" in
		--force-config) FORCE_CONFIG=1 ;;
		--no-start)     NO_START=1 ;;
		-h|--help)      usage; exit 0 ;;
		*)              echo "unknown option: $arg" >&2; usage >&2; exit 2 ;;
	esac
done

if [ "$(id -u)" -ne 0 ]; then
	echo "run as root: sudo ./install.sh" >&2
	exit 1
fi

if ! command -v systemctl >/dev/null 2>&1 || [ ! -d /run/systemd/system ]; then
	echo "systemd is required (systemctl not found or systemd is not running)" >&2
	exit 1
fi

if [ ! -e /dev/uinput ]; then
	echo "/dev/uinput is missing. Load the module first:" >&2
	echo "  sudo modprobe uinput" >&2
	exit 1
fi

if command -v make >/dev/null 2>&1 && command -v cc >/dev/null 2>&1 && [ -f "$SCRIPT_DIR/Makefile" ]; then
	echo "building LinAppleKey..."
	make -C "$SCRIPT_DIR" clean all
	BIN="$SCRIPT_DIR/build/linapplekeyd"

	# make runs as root here; return the build tree to the invoking user
	# so a later "make clean" works without sudo.
	if [ -n "${SUDO_UID:-}" ] && [ -n "${SUDO_GID:-}" ]; then
		chown -R "$SUDO_UID:$SUDO_GID" "$SCRIPT_DIR/build" 2>/dev/null || true
	fi
elif [ -x "$SCRIPT_DIR/linapplekeyd" ]; then
	BIN="$SCRIPT_DIR/linapplekeyd"
elif [ -x "$SCRIPT_DIR/build/linapplekeyd" ]; then
	BIN="$SCRIPT_DIR/build/linapplekeyd"
else
	echo "no prebuilt binary and no make/cc; install build tools first" >&2
	exit 1
fi

if [ ! -x "$BIN" ]; then
	echo "cannot find or build linapplekeyd" >&2
	exit 1
fi

"$BIN" --version

# Stop the old instance before replacing the binary. Linux would keep the
# old inode alive, but a clean stop avoids mixing versions.
if systemctl is-active --quiet linapplekeyd.service 2>/dev/null; then
	systemctl stop linapplekeyd.service
fi

install -Dm755 "$BIN" /usr/local/bin/linapplekeyd
install -Dm644 "$SCRIPT_DIR/linapplekeyd.service" /etc/systemd/system/linapplekeyd.service
install -Dm644 "$SCRIPT_DIR/README_RU.md" /usr/local/share/doc/linapplekey/README_RU.md
install -Dm644 "$SCRIPT_DIR/README.md" /usr/local/share/doc/linapplekey/README.md
install -Dm644 "$SCRIPT_DIR/linapplekey.conf.example" /usr/local/share/doc/linapplekey/linapplekey.conf.example

if [ -e /etc/linapplekey.conf ] && [ "$FORCE_CONFIG" -eq 0 ]; then
	echo "keeping existing /etc/linapplekey.conf (use --force-config to replace)"
else
	install -Dm644 "$SCRIPT_DIR/linapplekey.conf.example" /etc/linapplekey.conf
	echo "installed /etc/linapplekey.conf"
fi

systemctl daemon-reload

if [ "$NO_START" -eq 0 ]; then
	if ! systemctl enable --now linapplekeyd.service; then
		echo "service failed to start; see:" >&2
		echo "  systemctl status linapplekeyd.service" >&2
		echo "  journalctl -u linapplekeyd -b" >&2
		exit 1
	fi

	echo
	echo "LinAppleKey installed and running."
	echo "Logs:    journalctl -u linapplekeyd -f"
	echo "Config:  /etc/linapplekey.conf"
	echo "Reload:  sudo systemctl reload linapplekeyd"
else
	echo "LinAppleKey installed (service not started)."
	echo "Start:   sudo systemctl enable --now linapplekeyd"
fi
