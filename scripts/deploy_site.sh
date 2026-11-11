#!/usr/bin/env bash
# Upload site/ to the lan-mic.suxitech.cn web directory over ssh.
# The remote host has no rsync, so stream a tar (newest files overwrite, others stay).
#
# Usage:
#   ./deploy_site.sh

set -euo pipefail

HOST="root@106.14.210.15"
REMOTE_DIR="/opt/1panel/apps/openresty/openresty/www/sites/lan-mic.suxitech.cn/index"
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
SRC="$SCRIPT_DIR/../site"

# the site dir is owned by 1panel on the server; keep it that way after a root extract
tar -czf - -C "$SRC" --exclude .DS_Store . \
  | ssh "$HOST" "tar -xzf - -C '$REMOTE_DIR' && chown -R 1panel:1panel '$REMOTE_DIR'"

echo "deployed $SRC -> $HOST:$REMOTE_DIR"
