"""Summarizes the Fig. 19 reproduction (validation/run_paper_fig19.sh) and compares it with the values reported in the paper.

    python validation/summarize_fig19.py validation/fig19_original_driver > validation/RESULTS_fig19.md

For every run, the last line of energy.txt (written by the original optimizer) is the accepted final state:
volume fraction, compliance 1/2 u^T K u, volume penalty, total energy.
"""

import re
import sys
from pathlib import Path

# Fig. 19 columns -> caseID of src/tests/shapeOpt/shapeOpt.cpp, and the energies printed under the figure
PAPER = {  # caseID: (column, E ours (msd), E medial axis (ma))
    0: (1, 1.76, 1.89),
    1: (2, 1.73, 1.90),
    5: (3, 1.71, 2.53),
    3: (4, 1.85, 2.00),
}


def final_state(run_dir: Path):
    energy = run_dir / "energy.txt"
    log = run_dir / "log.txt"
    if not energy.is_file():
        return None
    lines = [l.strip() for l in energy.read_text().splitlines() if l.strip()]
    if not lines:
        return None
    vol, compliance, penalty, total = map(float, lines[-1].split(","))
    text = log.read_text(errors="replace") if log.is_file() else ""
    iterations = len(re.findall(r"^Iter: ", text, flags=re.M))
    changes = re.findall(r"^max change: ([0-9.eE+-]+)", text, flags=re.M)
    converged = bool(changes) and float(changes[-1]) < 1e-5
    return dict(vol=vol, compliance=compliance, penalty=penalty, total=total, iterations=iterations, converged=converged)


def main() -> None:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else "validation/fig19_original_driver")
    print("| Fig. 19 column | caseID | method | paper E | compliance (ours) | rel. diff | volume fraction | total energy | iterations |")
    print("|---|---|---|---|---|---|---|---|---|")
    for method, name in ((0, "msd (ours)"), (1, "ma (medial axis)")):
        for case, (column, e_msd, e_ma) in PAPER.items():
            st = final_state(root / f"case{case}_method{method}")
            paper = e_msd if method == 0 else e_ma
            if st is None:
                print(f"| {column} | {case} | {name} | {paper:.2f} | (not run) | | | | |")
                continue
            rel = (st["compliance"] - paper) / paper * 100
            it = f"{st['iterations']}{' (converged)' if st['converged'] else ''}"
            print(f"| {column} | {case} | {name} | {paper:.2f} | {st['compliance']:.4f} | {rel:+.1f}% | {st['vol']:.4f} | "
                  f"{st['total']:.4f} | {it} |")


if __name__ == "__main__":
    main()
