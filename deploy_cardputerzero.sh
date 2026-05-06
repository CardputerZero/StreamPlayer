#!/usr/bin/env bash
set -euo pipefail

HOST="${1:-${CARDPUTER_ZERO_HOST:-}}"
SSH_USER="${2:-${CARDPUTER_ZERO_USER:-}}"
if [ -z "$HOST" ] || [ -z "$SSH_USER" ]; then
  echo "usage: $0 <host> <ssh-user> [app-dir]" >&2
  echo "or set CARDPUTER_ZERO_HOST and CARDPUTER_ZERO_USER" >&2
  exit 2
fi
APP_DIR="${3:-${CARDPUTER_ZERO_APP_DIR:-/home/${SSH_USER}/apps/streamplayer}}"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
export PATH="/opt/homebrew/bin:/usr/local/bin:${PATH}"

cd "$(dirname "$0")"

if [ ! -d static_lib ] && [ -f ../../SDK/github_source/static_lib_v0.0.1.tar.gz ]; then
  mkdir -p static_lib
  tar -xzf ../../SDK/github_source/static_lib_v0.0.1.tar.gz -C static_lib
fi

if [ -d static_lib/usr/include/aarch64-linux-gnu ] && [ ! -e static_lib/usr/include/aarch64-unknown-linux-gnu ]; then
  ln -s aarch64-linux-gnu static_lib/usr/include/aarch64-unknown-linux-gnu
fi

if [ -d static_lib/usr/lib/aarch64-linux-gnu ] && [ ! -e static_lib/usr/lib/aarch64-unknown-linux-gnu ]; then
  ln -s aarch64-linux-gnu static_lib/usr/lib/aarch64-unknown-linux-gnu
fi

export CPATH="${PWD}/static_lib/usr/include/aarch64-linux-gnu:${CPATH:-}"
export C_INCLUDE_PATH="${PWD}/static_lib/usr/include/aarch64-linux-gnu:${C_INCLUDE_PATH:-}"
export CPLUS_INCLUDE_PATH="${PWD}/static_lib/usr/include/aarch64-linux-gnu:${CPLUS_INCLUDE_PATH:-}"
export LIBRARY_PATH="${PWD}/static_lib/usr/lib/aarch64-linux-gnu:${LIBRARY_PATH:-}"

if [ -f build/config/config_tmp.mk ] && ! grep -q "CONFIG_V9_5_LV_USE_LINUX_FBDEV" build/config/config_tmp.mk; then
  scons distclean
fi

if [ -f build/config/config_tmp.mk ] && command -v aarch64-linux-gnu-gcc >/dev/null 2>&1 && ! grep -q "CONFIG_TOOLCHAIN_PATH" build/config/config_tmp.mk; then
  scons distclean
fi

if [ -f build/config/config_tmp.mk ] && ! grep -q "CONFIG_TOOLCHAIN_FLAGS" build/config/config_tmp.mk; then
  scons distclean
fi

if [ -f build/config/config_tmp.mk ] && ! grep -q -- "-B.*/usr/lib/aarch64-linux-gnu" build/config/config_tmp.mk; then
  scons distclean
fi

CONFIG_REPO_AUTOMATION=1 CardputerZero=y scons -j"${JOBS}"
ssh "${SSH_USER}@${HOST}" "mkdir -p '${APP_DIR}/dist'"
rsync -az --delete dist/ "${SSH_USER}@${HOST}:${APP_DIR}/dist/"
ssh "${SSH_USER}@${HOST}" "chmod +x '${APP_DIR}/dist/M5CardputerZero-StreamPlayer'"
