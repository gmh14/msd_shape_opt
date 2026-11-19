"""Figure of the Fig. 19 reproduction (validation/run_paper_fig19.sh): final shapes (front view) and convergence.

    /venv/msd_shape_opt/bin/python validation/plot_fig19.py validation/fig19_original_driver validation/fig19_reproduction.png
"""

import re
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import matplotlib.ticker  # noqa: E402
import numpy as np  # noqa: E402

RES = (60, 40, 6)
CELL = 0.01
COLUMNS = [(0, 1, 1.76, 1.89), (1, 2, 1.73, 1.90), (5, 3, 1.71, 2.53), (3, 4, 1.85, 2.00)]  # caseID, column, E msd, E ma

# reference palette (dataviz skill, light mode): categorical slots 1-2, surface and text tokens
SURFACE = "#fcfcfb"
TEXT = "#0b0b0b"
TEXT2 = "#52514e"
GRID = "#e4e3df"
SERIES = {"msd": "#2a78d6", "ma": "#eb6834"}


def voxels(run_dir: Path) -> np.ndarray:
    """Occupancy grid from the final voxel OBJ written by the original optimizer (8 vertices per box, first = min corner)."""
    occ = np.zeros(RES, dtype=bool)
    verts = [l for l in (run_dir / "final-sample.obj").read_text().splitlines() if l.startswith("v ")]
    for i in range(0, len(verts), 8):
        x, y, z = (float(t) for t in verts[i].split()[1:4])
        ix, iy, iz = (int(round(c / CELL)) for c in (x, y, z))
        occ[ix, iy, iz] = True
    return occ


def accepted_energies(run_dir: Path) -> np.ndarray:
    """Objective (compliance + volume penalty) of the initial state and after every accepted step, from log.txt."""
    blocks = (run_dir / "log.txt").read_text(errors="replace").split("Iter: ")[1:]
    pat = re.compile(r"E: ([-0-9.eE+]+),([-0-9.eE+]+)")
    out = []
    for k, b in enumerate(blocks):
        found = pat.findall(b)
        if not found:
            continue
        if k == 0:
            out.append(sum(map(float, found[0])))
        if "max change" in b:
            out.append(sum(map(float, found[-1])))
    return np.array(out)


def final_compliance(run_dir: Path) -> float:
    last = [l for l in (run_dir / "energy.txt").read_text().splitlines() if l.strip()][-1]
    return float(last.split(",")[1])


def main() -> None:
    root = Path(sys.argv[1] if len(sys.argv) > 1 else "validation/fig19_original_driver")
    out = Path(sys.argv[2] if len(sys.argv) > 2 else "validation/fig19_reproduction.png")

    plt.rcParams.update({"font.size": 9, "text.color": TEXT, "axes.labelcolor": TEXT2, "xtick.color": TEXT2,
                         "ytick.color": TEXT2, "axes.edgecolor": GRID, "figure.facecolor": SURFACE,
                         "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE})
    fig, axes = plt.subplots(3, 4, figsize=(13, 8.2), gridspec_kw={"height_ratios": [1, 1, 1.15]})
    gray = matplotlib.colors.LinearSegmentedColormap.from_list("depth", [SURFACE, "#5f5e5a"])

    for col, (case, column, e_msd, e_ma) in enumerate(COLUMNS):
        for row, (method, paper) in enumerate((("msd", e_msd), ("ma", e_ma))):
            ax = axes[row, col]
            d = root / f"case{case}_method{0 if method == 'msd' else 1}"
            occ = voxels(d)
            depth = occ.sum(axis=2).T / RES[2]  # front view (along z): filled fraction of the thickness
            ax.imshow(depth, origin="lower", cmap=gray, vmin=0, vmax=1, interpolation="nearest",
                      extent=(0, RES[0], 0, RES[1]))
            ax.add_patch(plt.Rectangle((0, 0), RES[0], RES[1], fill=False, ec=GRID, lw=1))
            ax.set_xticks([])
            ax.set_yticks([])
            for s in ax.spines.values():
                s.set_visible(False)
            e = final_compliance(d)
            ax.set_title(f"column {column} · {method}\nE = {e:.3f}   (paper {paper:.2f}, {100 * (e - paper) / paper:+.1f}%)",
                         fontsize=9, color=TEXT)
            if col == 0:
                ax.text(-6, RES[1] / 2, "ours (msd)" if method == "msd" else "medial axis (ma)", rotation=90,
                        va="center", ha="center", color=TEXT2, fontsize=9)

        ax = axes[2, col]
        curves = {m: accepted_energies(root / f"case{case}_method{0 if m == 'msd' else 1}") for m in ("msd", "ma")}
        upper = max(curves, key=lambda m: curves[m][-1])  # direct label of the higher end point goes above it
        for method, e in curves.items():
            ax.plot(np.arange(len(e)), e, color=SERIES[method], lw=2, label=method, solid_capstyle="round")
            ax.annotate(method, (len(e) - 1, e[-1]), xytext=(4, 5 if method == upper else -11), textcoords="offset points",
                        color=TEXT2, fontsize=8)
        ax.set_yscale("log")
        ticks = [t for t in (1, 1.5, 2, 3, 5, 10, 20, 40) if t <= max(c.max() for c in curves.values()) * 1.05]
        ax.set_yticks(ticks)
        ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f"{v:g}"))
        ax.yaxis.set_minor_formatter(matplotlib.ticker.NullFormatter())
        ax.set_xlim(0, 215)
        ax.grid(True, which="major", color=GRID, lw=0.6)
        ax.set_xlabel("iteration")
        if col == 0:
            ax.set_ylabel("objective (compliance + volume penalty)")
        for s in ("top", "right"):
            ax.spines[s].set_visible(False)

    handles, labels = axes[2, 0].get_legend_handles_labels()
    fig.legend(handles, ["ours (msd)", "medial axis (ma)"], loc="lower center", ncol=2, frameon=False,
               bbox_to_anchor=(0.5, -0.005))
    fig.suptitle("Reproduction of Fig. 19 with the original driver (src/tests/shapeOpt/shapeOpt.cpp) and the reconstructed "
                 "templates: front view of the final voxels\n(gray level = filled fraction of the 6-voxel thickness), "
                 "compliance E = 1/2 u^T K u, and convergence", fontsize=10, color=TEXT)
    fig.tight_layout(rect=(0, 0.03, 1, 0.95))
    fig.savefig(out, dpi=130)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
