# Web API and run-directory format

The web interface is a single-page app (`web/static`) served by `web/server.py` (FastAPI + uvicorn).
The server queues optimization jobs, runs each one as a separate `msdShapeOpt` process, and exposes the
run directories through a small REST API plus a Server-Sent-Events stream for live progress.

All lengths in the run files are **world coordinates** (`world = grid * cell_size`, `cell_size = 0.01`).
Grid coordinates are integers `0..n` along each axis for the grid vertices; voxel `(x, y, z)` spans
`[x, x+1] x [y, y+1] x [z, z+1]` in grid coordinates (its center is at `(x+0.5, y+0.5, z+0.5)`).

## Run directory (`runs/<run_id>/`)

| file | written by | content |
|---|---|---|
| `input.json` | server | the configuration submitted by the user |
| `status.json` | server | `{id, name, state, created, started, finished, exit_code, error, method, num_voxels, max_iterations, threads, paper_reference}`; `state` is one of `queued`, `running`, `completed`, `failed`, `cancelled`, `interrupted` |
| `stdout.log` | server | stdout + stderr of `msdShapeOpt` (includes the original optimizer's console output) |
| `config.json` | driver | normalized configuration (defaults filled in) |
| `meta.json` | driver | everything needed to display the problem (see below) |
| `history.jsonl` | driver | one JSON object per reported iteration (appended + flushed as the run progresses) |
| `frames/frame_XXXX.json` | driver | `{iteration, vertices, primitive_positions}` for iteration `XXXX` (4 digits, zero padded) |
| `frames/theta_XXXX.u8` | driver | one byte per voxel: `round(255 * (theta - 0.01) / 0.99)`; the shape is `theta > 0.5` (byte value `>= 128`) |
| `result.json` | driver | written at the end: `{status: "completed", converged, iterations, final: <last history record>, elapsed, files}` |
| `final_voxels.obj`, `final_primitives.obj`, `final_skeleton.obj` | driver | final shape as voxels (the original code's output), as the union of the primitive meshes, and the skeletal graph (`l` elements) |
| `energy.txt`, `initial-sample.obj`, `iterXXXX-*.obj`, `s.obj`, `c1.obj` | original optimizer | the original code's own outputs and the template primitive meshes |

### `meta.json`

```jsonc
{
  "format_version": 1,
  "name": "fig19_case0", "description": "...",
  "grid": {"resolution": [60, 40, 6], "cell_size": 0.01, "num_voxels": 14400, "num_grid_vertices": 17507,
           "voxel_index": "index = (x * ny + y) * nz + z; ..."},
  "skeleton": {"vertices": [[x, y, z], ...],          // initial positions (world)
               "edges": [[i, j], ...],                 // as used by the optimizer
               "flipped_edges": [k, ...]},             // edges whose end points were swapped (see README)
  "boundary_conditions": {
      "fixed_vertices": [[i, j, k], ...],              // integer grid coordinates of the clamped grid vertices
      "loads": [{"vertex": [i, j, k], "force": [fx, fy, fz]}, ...],
      "total_force": [fx, fy, fz]},
  "optimization": {"method": "msd" | "ma", "method_id": 0 | 1, "volume_fraction": 0.2, "max_iterations": 200,
                   "initial_radius_voxels": 4.0, "initial_radius": 0.04, "convergence_threshold": 1e-05,
                   "young_modulus": 1e6, "poisson_ratio": 0.45, "theta_min": 0.01},
  "primitives": {"num_vertex_primitives": 6, "num_edge_primitives": 8,
                 "sphere_template":   {"num_vertices": 8,  "triangles": [[a, b, c], ...]},
                 "cylinder_template": {"num_vertices": 14, "triangles": [[a, b, c], ...]},
                 "positions_layout": "vertex primitives first, then edge primitives"},
  "theta_encoding": "...",
  "warnings": ["..."]
}
```

### `history.jsonl` record (also the payload of the SSE `iteration` event)

```jsonc
{"iteration": 12,                 // 0 = initial state, k = after the k-th accepted gradient step
 "compliance": 1.98,              // 1/2 u^T K u  (the "E" reported in the paper)
 "volume_penalty": 0.04,          // w/2 max(V - V0, 0)^2
 "energy": 2.02,                  // compliance + volume_penalty (the objective)
 "volume_fraction": 0.206,        // filled fraction of the voxels (target: optimization.volume_fraction)
 "filled_voxels": 2970,           // voxels with theta > 0.5
 "gradient_norm": 12.3, "step_size": 1.2e-4, "max_change": 3e-4,   // convergence when max_change < 1e-5
 "line_search_iterations": 2,
 "radius_min_voxels": 1.0, "radius_max_voxels": 6.3,
 "elapsed": 25.1,                 // seconds since the optimization started
 "vertices": [[x, y, z], ...]}    // current skeleton vertex positions (world)
```

### `frames/frame_XXXX.json`

`primitive_positions` is a flat array `[x0, y0, z0, x1, ...]` of all template-mesh vertices of all primitives
(vertex primitives first, `sphere_template.num_vertices` points each, then edge primitives,
`cylinder_template.num_vertices` points each). Combined with the template triangles of `meta.json`, they give
the generalized enveloping primitives whose union is the optimized shape.

## REST API

| method & path | description |
|---|---|
| `GET /api/health` | `{ok, binary, binary_exists, runs_dir, cpu_count, running, queued, max_concurrent}` |
| `GET /api/examples` | `[{id, name, description, resolution, num_vertices, num_edges, num_voxels, paper_reference}]` |
| `GET /api/examples/{id}` | the example configuration (JSON) |
| `POST /api/validate` | body: a configuration (JSON). Returns `{ok: true, warnings, meta}` or `{ok: false, error}` (HTTP 200 in both cases) |
| `POST /api/runs` | body: `{"config": {...}, "threads": n?}`. Validates, creates the run, queues it. Returns `{id, status}`; HTTP 400 with `{detail}` if invalid |
| `GET /api/runs` | `[{...status.json, iteration, compliance, energy, volume_fraction, elapsed}]`, newest first |
| `GET /api/runs/{id}` | `{status, config, meta, history: [records], result, error}` (`meta`/`result` may be `null` while queued/running) |
| `GET /api/runs/{id}/events?since=k` | SSE stream: `status` (the status object, sent at start and on change), `iteration` (history records with `iteration > k`), `end` (terminal state reached, then the stream closes). Comment keep-alives every 15 s |
| `GET /api/runs/{id}/frames/{k}` | `frames/frame_k.json` |
| `GET /api/runs/{id}/theta/{k}` | `frames/theta_k.u8` (`application/octet-stream`) |
| `GET /api/runs/{id}/log?tail=n` | `{lines: [...]}` last `n` lines of `stdout.log` |
| `GET /api/runs/{id}/files/{name}` | download one of `final_voxels.obj, final_primitives.obj, final_skeleton.obj, history.jsonl, energy.txt, config.json, input.json, meta.json, result.json, stdout.log, initial-sample.obj` |
| `POST /api/runs/{id}/cancel` | stop a queued or running job |
| `DELETE /api/runs/{id}` | delete a finished run (HTTP 409 while it is queued/running) |
