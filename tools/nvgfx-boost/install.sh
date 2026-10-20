#!/bin/bash
# <wtl> fork: nvgfx | author: watchthelight </wtl>
# Install nvgfx-boost (run with sudo): the daemon, its LaunchDaemon, and BOOST=0 in nvrm610.conf so the daemon is the
# only boost holder. Takes full effect after a restart (WindowServer reads nvrm610.conf when it starts).
# Uninstall: install.sh --remove (puts BOOST back to the default by deleting the line).
set -euo pipefail
cd "$(dirname "$0")"
CONF=/Library/GPUBundles/nvmtl/nvrm610.conf
if [ "${1:-}" = "--remove" ]; then
  launchctl bootout system/org.watchthelight.nvgfx-boost 2>/dev/null || true
  rm -f /Library/LaunchDaemons/org.watchthelight.nvgfx-boost.plist /usr/local/libexec/nvgfx-boost
  [ -f "$CONF" ] && sed -i '' '/^BOOST=/d' "$CONF"
  echo "removed"; exit 0
fi
[ -x nvgfx-boost ] || { echo "run build.sh first"; exit 1; }
install -d -m 755 /usr/local/libexec
install -m 755 -o root -g wheel nvgfx-boost /usr/local/libexec/nvgfx-boost
install -m 644 -o root -g wheel org.watchthelight.nvgfx-boost.plist /Library/LaunchDaemons/
if [ -f "$CONF" ]; then
  grep -q '^BOOST=' "$CONF" && sed -i '' 's/^BOOST=.*/BOOST=0/' "$CONF" || printf 'BOOST=0\n' >> "$CONF"
fi
launchctl bootout system/org.watchthelight.nvgfx-boost 2>/dev/null || true
launchctl bootstrap system /Library/LaunchDaemons/org.watchthelight.nvgfx-boost.plist
echo "installed; restart so WindowServer stops holding its own boost"
