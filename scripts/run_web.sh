#!/usr/bin/env bash
# Starts the web interface (foreground). Binds to 127.0.0.1 by default: reach it through an SSH tunnel
# (ssh -L 17860:127.0.0.1:17860 ...) or through an authenticating reverse proxy (see README.md, "Remote access").
#   usage: scripts/run_web.sh [--host 127.0.0.1] [--port 17860]
# Environment: MSD_CONDA_ENV (default msd_shape_opt), MSD_RUNS_DIR (default <repo>/runs), MSD_MAX_CONCURRENT (default 1),
#              MSD_THREADS (threads per run, default 0 = all cores), MSD_BIN (default <repo>/build/bin/msdShapeOpt)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ENV_NAME="${MSD_CONDA_ENV:-msd_shape_opt}"

PY=""
for cand in "/venv/${ENV_NAME}/bin/python" "/opt/miniforge3/envs/${ENV_NAME}/bin/python" "${CONDA_PREFIX:-/nonexistent}/bin/python"; do
  if [[ -x "$cand" ]] && "$cand" -c "import fastapi, uvicorn" 2>/dev/null; then PY="$cand"; break; fi
done
if [[ -z "$PY" ]]; then
  CONDA_EXE="${CONDA_EXE:-$(command -v conda || echo /opt/miniforge3/bin/conda)}"
  PY="$("$CONDA_EXE" run -n "$ENV_NAME" python -c 'import sys; print(sys.executable)')"
fi

if [[ ! -x "${MSD_BIN:-${ROOT}/build/bin/msdShapeOpt}" ]]; then
  echo "msdShapeOpt is not built yet: run scripts/build.sh first" >&2
  exit 1
fi
exec "$PY" "${ROOT}/web/server.py" "$@"
