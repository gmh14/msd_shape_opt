#!/usr/bin/env bash
# Configures and builds msd_shape_opt inside the conda env created by scripts/setup_env.sh.
#   usage: scripts/build.sh [Release|Debug]
set -euo pipefail

ENV_NAME="${MSD_CONDA_ENV:-msd_shape_opt}"
BUILD_TYPE="${1:-Release}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${MSD_BUILD_DIR:-${ROOT}/build}"

CONDA_EXE="${CONDA_EXE:-$(command -v conda || true)}"
if [[ -z "${CONDA_EXE}" && -x /opt/miniforge3/bin/conda ]]; then CONDA_EXE=/opt/miniforge3/bin/conda; fi
eval "$("${CONDA_EXE}" shell.bash hook)"
conda activate "${ENV_NAME}"

GENERATOR=()
if command -v ninja >/dev/null 2>&1; then GENERATOR=(-G Ninja); fi

cmake -S "${ROOT}" -B "${BUILD_DIR}" "${GENERATOR[@]}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" -DPGO_USE_MKL=ON -DPGO_CHECK_CONDA=ON
cmake --build "${BUILD_DIR}" --parallel "$(nproc)"

# template primitive meshes used by the optimizer
"${BUILD_DIR}/bin/makeTemplates" "${ROOT}/data/templates"
echo "Build finished: ${BUILD_DIR}/bin"
