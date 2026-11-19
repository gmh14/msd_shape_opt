#!/usr/bin/env bash
# Reproduces Fig. 19 of the paper with the ORIGINAL (unmodified) driver src/tests/shapeOpt/shapeOpt.cpp.
# Fig. 19 columns (left -> right) correspond to caseID 0, 1, 5, 3 in shapeOpt.cpp;
# method 0 = medial skeletal diagram ("Final Shape"), method 1 = medial axis baseline ("MA Final Shape").
#   usage: validation/run_paper_fig19.sh <output_dir> [build_dir]
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:?output dir}"
BIN="${2:-${ROOT}/build}/bin/shapeOpt"
for method in 0 1; do
  for caseID in 0 1 5 3; do
    d="${OUT}/case${caseID}_method${method}"
    mkdir -p "$d"
    cp "${ROOT}/data/templates/s.obj" "${ROOT}/data/templates/c1.obj" "$d/"
    echo "[$(date +%T)] running case ${caseID} method ${method}"
    ( cd "$d" && "${BIN}" "${caseID}" "${method}" > log.txt 2>&1 )
    echo "[$(date +%T)] done (exit $?)"
    last=$(ls "$d"/iter*-sample.obj 2>/dev/null | tail -1); [ -n "$last" ] && cp "$last" "$d/final-sample.obj"
    rm -f "$d"/iter*-sample.obj "$d"/r.obj   # large per-iteration voxel OBJs (the last one was kept as final-sample.obj)
  done
done
