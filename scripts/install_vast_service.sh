#!/usr/bin/env bash
# Installs the web interface as a supervisor service on a Vast.ai base-image instance and exposes it through the
# instance's authenticating Caddy edge (token auth) on a free external port (see /workspace/CLAUDE.md, section 7).
#   usage: scripts/install_vast_service.sh [external_port=10100] [internal_port=17860]
# Afterwards the interface is at  http://$PUBLIC_IPADDR:$VAST_TCP_PORT_<external_port>/?token=$OPEN_BUTTON_TOKEN
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXT_PORT="${1:-10100}"
INT_PORT="${2:-17860}"
LABEL="MSD Shape Optimization"

if [[ -z "$(printenv "VAST_TCP_PORT_${EXT_PORT}" || true)" ]]; then
  echo "VAST_TCP_PORT_${EXT_PORT} is not set: ${EXT_PORT} is not an open port of this instance (see: vast-capabilities | jq '.instance.open_ports')" >&2
  exit 1
fi

install -m 755 "${ROOT}/scripts/supervisor/msd-shape-opt.sh" /opt/supervisor-scripts/msd-shape-opt.sh
sed -i "s|/workspace/Projects/msd_shape_opt|${ROOT}|; s|MSD_PORT:-17860|MSD_PORT:-${INT_PORT}|" /opt/supervisor-scripts/msd-shape-opt.sh
install -m 644 "${ROOT}/scripts/supervisor/msd-shape-opt.conf" /etc/supervisor/conf.d/msd-shape-opt.conf

python3 - "$LABEL" "$EXT_PORT" "$INT_PORT" <<'PY'
import sys, yaml
label, ext, internal = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
path = "/etc/portal.yaml"
d = yaml.safe_load(open(path)) or {}
apps = d.setdefault("applications", {}) or {}
d["applications"] = apps
for name, app in apps.items():
    if name != label and int(app.get("external_port", -1)) == ext:
        sys.exit(f"external port {ext} is already used by '{name}'")
apps[label] = {"hostname": "localhost", "external_port": ext, "internal_port": internal, "open_path": "/", "name": label}
yaml.safe_dump(d, open(path, "w"), sort_keys=False)
PY

rm -f /tmp/supervisor-skip/msd-shape-opt 2>/dev/null || true
supervisorctl reread
supervisorctl update
supervisorctl restart msd-shape-opt || supervisorctl start msd-shape-opt
supervisorctl restart caddy
echo "Installed: app on 127.0.0.1:${INT_PORT}; external port ${EXT_PORT} -> public port $(printenv "VAST_TCP_PORT_${EXT_PORT}")"
