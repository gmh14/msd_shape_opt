#!/usr/bin/env bash
# Creates the conda environment used to build and run msd_shape_opt.
# Following libpgo (https://github.com/bohanwang/libpgo#windows--ubuntu), MKL and TBB come from conda:
#     conda install tbb tbb-devel mkl mkl-devel
# All other C++ dependencies are fetched by CMake (CMakeModules/third-party/*.cmake, borrowed from libpgo).
# System prerequisites (as in libpgo): cmake >= 3.28, gcc 11-13, ninja (optional), libgmp-dev, libmpfr-dev.
set -euo pipefail

ENV_NAME="${MSD_CONDA_ENV:-msd_shape_opt}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

CONDA_EXE="${CONDA_EXE:-$(command -v conda || true)}"
if [[ -z "${CONDA_EXE}" && -x /opt/miniforge3/bin/conda ]]; then CONDA_EXE=/opt/miniforge3/bin/conda; fi
if [[ -z "${CONDA_EXE}" ]]; then echo "conda not found" >&2; exit 1; fi
SOLVER="$(dirname "${CONDA_EXE}")/mamba"; [[ -x "${SOLVER}" ]] || SOLVER="${CONDA_EXE}"

if ! "${CONDA_EXE}" env list | awk '{print $1}' | grep -qx "${ENV_NAME}"; then
  "${SOLVER}" create -y -n "${ENV_NAME}" -c conda-forge python=3.12 tbb tbb-devel mkl mkl-devel
else
  echo "conda env '${ENV_NAME}' already exists"
fi

# python packages for the web interface
"${CONDA_EXE}" run -n "${ENV_NAME}" python -m pip install -q -r "${ROOT}/web/requirements.txt"

for lib in gmp mpfr; do
  if ! ldconfig -p | grep -q "lib${lib}.so"; then
    echo "WARNING: lib${lib} not found. Install it with: sudo apt install lib${lib}-dev" >&2
  fi
done
echo "Environment '${ENV_NAME}' is ready."
