"""Web interface for msd_shape_opt (topology-constrained shape optimization with the Medial Skeletal Diagram).

Serves the single-page app in ./static and a small REST/SSE API (see docs/WEB_API.md). Every optimization runs
as a separate `msdShapeOpt` process whose output directory (runs/<run_id>/) is the single source of truth.

    python web/server.py [--host 127.0.0.1] [--port 17860]

Environment variables: MSD_BIN (path of msdShapeOpt), MSD_RUNS_DIR, MSD_MAX_CONCURRENT (default 1),
MSD_THREADS (threads per run, default 0 = all cores), MSD_MAX_VOXELS (hard grid limit, default 5M), MSD_HOST, MSD_PORT.
"""

from __future__ import annotations

import argparse
import asyncio
import collections
import json
import os
import re
import shutil
import signal
import subprocess
import tempfile
import threading
import time
import traceback
import uuid
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import FileResponse, JSONResponse, Response, StreamingResponse
from fastapi.staticfiles import StaticFiles

ROOT = Path(__file__).resolve().parent.parent
STATIC_DIR = Path(__file__).resolve().parent / "static"
EXAMPLES_DIR = ROOT / "examples"
RUNS_DIR = Path(os.environ.get("MSD_RUNS_DIR", ROOT / "runs")).resolve()
BIN = Path(os.environ.get("MSD_BIN", ROOT / "build" / "bin" / "msdShapeOpt")).resolve()
MAX_CONCURRENT = max(1, int(os.environ.get("MSD_MAX_CONCURRENT", "1")))
DEFAULT_THREADS = max(0, int(os.environ.get("MSD_THREADS", "0")))
CPU_COUNT = os.cpu_count() or 1

MAX_CONFIG_BYTES = 1 << 20
RUN_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
EXAMPLE_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
TERMINAL_STATES = {"completed", "failed", "cancelled", "interrupted"}
DOWNLOADABLE = {
    "final_voxels.obj", "final_primitives.obj", "final_skeleton.obj", "history.jsonl", "energy.txt",
    "config.json", "input.json", "meta.json", "result.json", "stdout.log", "initial-sample.obj",
}

RUNS_DIR.mkdir(parents=True, exist_ok=True)


# ----------------------------------------------------------------------------------------------
# helpers
# ----------------------------------------------------------------------------------------------
def now() -> float:
    return time.time()


def read_json(path: Path) -> Any:
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def write_json_atomic(path: Path, data: Any) -> None:
    tmp = path.with_name(path.name + f".{uuid.uuid4().hex[:8]}.tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    os.replace(tmp, path)


def slugify(name: str) -> str:
    s = re.sub(r"[^A-Za-z0-9_-]+", "-", name or "run").strip("-_")
    return (s or "run")[:40]


def run_dir_of(run_id: str) -> Path:
    if not RUN_ID_RE.match(run_id):
        raise HTTPException(status_code=404, detail="unknown run")
    d = (RUNS_DIR / run_id).resolve()
    if d.parent != RUNS_DIR or not (d / "status.json").is_file():
        raise HTTPException(status_code=404, detail="unknown run")
    return d


def read_history(run_dir: Path) -> list[dict]:
    """All complete records of history.jsonl (a partially written last line is ignored)."""
    records = []
    try:
        with open(run_dir / "history.jsonl", "r", encoding="utf-8") as f:
            for line in f:
                if not line.endswith("\n"):
                    break
                line = line.strip()
                if line:
                    try:
                        records.append(json.loads(line))
                    except ValueError:
                        pass
    except OSError:
        pass
    return records


def last_history_record(run_dir: Path) -> dict | None:
    path = run_dir / "history.jsonl"
    try:
        size = path.stat().st_size
    except OSError:
        return None
    if size == 0:
        return None
    with open(path, "rb") as f:
        block = min(size, 1 << 16)
        f.seek(size - block)
        lines = f.read().split(b"\n")
    for line in reversed(lines[:-1]):  # the chunk after the last newline is incomplete (or empty)
        line = line.strip()
        if line:
            try:
                return json.loads(line)
            except ValueError:
                return None
    return None


def tail_lines(path: Path, n: int) -> list[str]:
    try:
        size = path.stat().st_size
    except OSError:
        return []
    with open(path, "rb") as f:
        block = min(size, max(4096, n * 400))
        f.seek(size - block)
        data = f.read().decode("utf-8", errors="replace")
    lines = data.splitlines()
    if block < size and lines:
        lines = lines[1:]
    return lines[-n:]


def extract_error(run_dir: Path) -> str | None:
    """Last error reported by msdShapeOpt (an '@@MSD {"event": "error"}' line) or the tail of the log."""
    lines = tail_lines(run_dir / "stdout.log", 400)
    for line in reversed(lines):
        if line.startswith("@@MSD "):
            try:
                ev = json.loads(line[6:])
            except ValueError:
                continue
            if ev.get("event") == "error":
                return ev.get("message")
    for line in reversed(lines):
        if "failed. Abort" in line or line.startswith("error:") or "terminate called" in line:
            return line.strip()
    return None


MAX_VOXELS = int(os.environ.get("MSD_MAX_VOXELS", "5000000"))  # hard cap, also with allow_large_grid
VALIDATE_SLOTS = threading.BoundedSemaphore(2)


def validate_config(config: Any) -> dict:
    """Runs `msdShapeOpt --validate-only` on the configuration. Returns {ok, error | warnings, meta}."""
    if not isinstance(config, dict):
        return {"ok": False, "error": "the configuration must be a JSON object"}
    res = config.get("grid", {}).get("resolution") if isinstance(config.get("grid"), dict) else None
    if isinstance(res, list) and len(res) == 3 and all(isinstance(n, (int, float)) for n in res):
        if res[0] * res[1] * res[2] > MAX_VOXELS:
            return {"ok": False, "error": f"grid.resolution has more than {MAX_VOXELS} voxels (server limit MSD_MAX_VOXELS)"}
    text = json.dumps(config)
    if len(text) > MAX_CONFIG_BYTES:
        return {"ok": False, "error": "the configuration is too large"}
    if not BIN.exists():
        return {"ok": False, "error": f"the optimizer binary {BIN} does not exist; build the project first"}

    tmp_dir = RUNS_DIR / ".validate"
    tmp_dir.mkdir(exist_ok=True)
    with tempfile.NamedTemporaryFile("w", suffix=".json", dir=tmp_dir, delete=False) as f:
        f.write(text)
        tmp_path = f.name
    try:
        with VALIDATE_SLOTS:
            proc = subprocess.run([str(BIN), tmp_path, "--validate-only"], capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        return {"ok": False, "error": "validation timed out"}
    finally:
        try:
            os.unlink(tmp_path)
        except OSError:
            pass

    out = proc.stdout.strip().splitlines()
    for line in reversed(out):
        try:
            res = json.loads(line)
            if isinstance(res, dict) and "ok" in res:
                return res
        except ValueError:
            continue
    return {"ok": False, "error": (proc.stderr or proc.stdout or "validation failed").strip()[-2000:]}


# ----------------------------------------------------------------------------------------------
# job manager
# ----------------------------------------------------------------------------------------------
class JobManager:
    def __init__(self) -> None:
        self.cond = threading.Condition()
        self.queue: collections.deque[str] = collections.deque()
        self.procs: dict[str, subprocess.Popen] = {}
        self.cancel_requested: set[str] = set()
        self.status_lock = threading.Lock()

    # status.json is written by the server only
    def update_status(self, run_id: str, **fields: Any) -> dict:
        with self.status_lock:
            path = RUNS_DIR / run_id / "status.json"
            status = read_json(path) or {"id": run_id}
            status.update(fields)
            write_json_atomic(path, status)
            return status

    def start(self) -> None:
        self.recover()
        for i in range(MAX_CONCURRENT):
            threading.Thread(target=self.worker, name=f"msd-worker-{i}", daemon=True).start()

    def recover(self) -> None:
        """After a restart: re-queue queued runs, re-attach to (or mark as interrupted) running ones."""
        entries = []
        for d in RUNS_DIR.iterdir():
            st = read_json(d / "status.json") if d.is_dir() else None
            if st:
                entries.append((st.get("created", 0), d.name, st))
        for _, run_id, st in sorted(entries):
            state = st.get("state")
            if state == "queued":
                self.queue.append(run_id)
            elif state == "running":
                pid = st.get("pid")
                if pid and self.pid_is_optimizer(pid):
                    threading.Thread(target=self.watch_orphan, args=(run_id, pid), daemon=True).start()
                elif (read_json(RUNS_DIR / run_id / "result.json") or {}).get("status") == "completed":
                    self.finalize(run_id, exit_code=None)
                else:
                    self.update_status(run_id, state="interrupted", finished=now(),
                                       error="the server stopped while this run was in progress")

    @staticmethod
    def pid_is_optimizer(pid: int) -> bool:
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                return b"msdShapeOpt" in f.read()
        except OSError:
            return False

    def watch_orphan(self, run_id: str, pid: int) -> None:
        while self.pid_is_optimizer(pid):
            time.sleep(1.0)
        self.finalize(run_id, exit_code=None)

    def submit(self, run_id: str) -> None:
        with self.cond:
            self.queue.append(run_id)
            self.cond.notify()

    def counts(self) -> tuple[int, int]:
        with self.cond:
            return len(self.procs), len(self.queue)

    def queue_position(self, run_id: str) -> int | None:
        with self.cond:
            try:
                return list(self.queue).index(run_id)
            except ValueError:
                return None

    def worker(self) -> None:
        while True:
            with self.cond:
                while not self.queue:
                    self.cond.wait()
                run_id = self.queue.popleft()
                cancelled = run_id in self.cancel_requested
                self.cancel_requested.discard(run_id)
            try:
                if cancelled:
                    self.update_status(run_id, state="cancelled", finished=now())
                else:
                    self.execute(run_id)
            except Exception as e:  # never let a worker die
                traceback.print_exc()
                try:
                    self.update_status(run_id, state="failed", finished=now(), error=f"server error: {e}")
                except Exception:
                    traceback.print_exc()

    def execute(self, run_id: str) -> None:
        run_dir = RUNS_DIR / run_id
        status = read_json(run_dir / "status.json") or {}
        if status.get("state") != "queued":
            return
        threads = int(status.get("threads") or DEFAULT_THREADS)
        cmd = [str(BIN), "input.json", "-o", str(run_dir)]
        if threads > 0:
            cmd += ["--threads", str(threads)]

        with open(run_dir / "stdout.log", "ab") as log:
            proc = subprocess.Popen(cmd, cwd=run_dir, stdout=log, stderr=subprocess.STDOUT,
                                    stdin=subprocess.DEVNULL, start_new_session=True)
            try:
                with self.cond:
                    self.procs[run_id] = proc
                self.update_status(run_id, state="running", started=now(), pid=proc.pid)
                # a cancel request could have arrived between the queue pop and Popen
                if run_id in self.cancel_requested:
                    self.terminate(proc)
                exit_code = proc.wait()
            except BaseException:
                self.terminate(proc)
                raise
            finally:
                with self.cond:
                    self.procs.pop(run_id, None)
        self.finalize(run_id, exit_code)

    def finalize(self, run_id: str, exit_code: int | None) -> None:
        run_dir = RUNS_DIR / run_id
        result = read_json(run_dir / "result.json")
        fields: dict[str, Any] = {"finished": now(), "exit_code": exit_code, "pid": None}
        if run_id in self.cancel_requested:
            self.cancel_requested.discard(run_id)
            fields.update(state="cancelled", error=None)
        elif result and result.get("status") == "completed":
            fields.update(state="completed", error=None)
        else:
            err = extract_error(run_dir)
            if exit_code is not None and exit_code < 0:
                try:
                    name = signal.Signals(-exit_code).name
                except ValueError:
                    name = str(-exit_code)
                err = (err + "; " if err else "") + f"the optimizer was terminated by signal {name}"
            elif exit_code not in (None, 0) and not err:
                err = f"the optimizer exited with code {exit_code}"
            fields.update(state="failed", error=err or "the optimizer stopped without writing result.json")
        self.update_status(run_id, **fields)

    @staticmethod
    def terminate(proc: subprocess.Popen) -> None:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except (ProcessLookupError, PermissionError):
            return

        def kill_later() -> None:
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except (ProcessLookupError, PermissionError):
                    pass

        threading.Thread(target=kill_later, daemon=True).start()

    def cancel(self, run_id: str) -> str:
        with self.cond:
            if run_id in self.queue:
                self.queue.remove(run_id)
                self.update_status(run_id, state="cancelled", finished=now())
                return "cancelled"
            proc = self.procs.get(run_id)
            status = read_json(RUNS_DIR / run_id / "status.json") or {}
            state = status.get("state")
            if proc is None and state not in ("queued", "running"):
                return state or "unknown"  # already finished: nothing to cancel
            # running, or popped from the queue but not started yet (execute() checks this flag after Popen)
            self.cancel_requested.add(run_id)
        if proc is not None:
            self.terminate(proc)
        elif state == "running" and status.get("pid") and self.pid_is_optimizer(status["pid"]):
            try:  # orphan of a previous server instance; watch_orphan() finalizes it
                os.killpg(status["pid"], signal.SIGTERM)
            except (ProcessLookupError, PermissionError):
                pass
        return "cancelling"


jobs = JobManager()


@asynccontextmanager
async def lifespan(_app: FastAPI):
    jobs.start()
    yield


app = FastAPI(title="MSD shape optimization", docs_url="/api/docs", openapi_url="/api/openapi.json",
              lifespan=lifespan)


# ----------------------------------------------------------------------------------------------
# API
# ----------------------------------------------------------------------------------------------
@app.get("/api/health")
def health() -> dict:
    running, queued = jobs.counts()
    return {"ok": True, "binary": str(BIN), "binary_exists": BIN.exists(), "runs_dir": str(RUNS_DIR),
            "cpu_count": CPU_COUNT, "running": running, "queued": queued, "max_concurrent": MAX_CONCURRENT,
            "default_threads": DEFAULT_THREADS}


@app.get("/api/examples")
def list_examples() -> list[dict]:
    out = []
    for p in sorted(EXAMPLES_DIR.glob("*.json")):
        cfg = read_json(p)
        if not isinstance(cfg, dict):
            continue
        res = (cfg.get("grid") or {}).get("resolution") or [0, 0, 0]
        sk = cfg.get("skeleton") or {}
        try:
            nvox = int(res[0]) * int(res[1]) * int(res[2])
        except (TypeError, ValueError, IndexError):
            nvox = None
        out.append({"id": p.stem, "name": cfg.get("name", p.stem), "description": cfg.get("description", ""),
                    "resolution": res, "num_vertices": len(sk.get("vertices") or []),
                    "num_edges": len(sk.get("edges") or []), "num_voxels": nvox,
                    "paper_reference": cfg.get("paper_reference")})
    return out


@app.get("/api/examples/{example_id}")
def get_example(example_id: str) -> JSONResponse:
    if not EXAMPLE_ID_RE.match(example_id):
        raise HTTPException(status_code=404, detail="unknown example")
    p = (EXAMPLES_DIR / f"{example_id}.json").resolve()
    if p.parent != EXAMPLES_DIR.resolve() or not p.is_file():
        raise HTTPException(status_code=404, detail="unknown example")
    return JSONResponse(read_json(p))


async def read_body_json(request: Request) -> Any:
    try:
        declared = int(request.headers.get("content-length") or 0)
    except ValueError:
        declared = 0
    if declared > MAX_CONFIG_BYTES:
        raise HTTPException(status_code=413, detail="request too large")
    body = await request.body()
    if len(body) > MAX_CONFIG_BYTES:
        raise HTTPException(status_code=413, detail="request too large")
    try:
        return json.loads(body)
    except ValueError as e:
        raise HTTPException(status_code=400, detail=f"invalid JSON: {e}")


@app.post("/api/validate")
async def validate(request: Request) -> dict:
    config = await read_body_json(request)
    return await asyncio.to_thread(validate_config, config)


@app.post("/api/runs")
async def create_run(request: Request) -> dict:
    body = await read_body_json(request)
    if not isinstance(body, dict) or "config" not in body:
        raise HTTPException(status_code=400, detail='expected {"config": {...}, "threads": n}')
    config = body["config"]
    threads = body.get("threads", DEFAULT_THREADS)
    try:
        threads = int(threads or 0)
    except (TypeError, ValueError):
        raise HTTPException(status_code=400, detail="threads must be an integer")
    threads = max(0, min(threads, CPU_COUNT))

    res = await asyncio.to_thread(validate_config, config)
    if not res.get("ok"):
        raise HTTPException(status_code=400, detail=res.get("error", "invalid configuration"))
    meta = res.get("meta") or {}

    name = str(config.get("name") or "run")
    run_id = f"{time.strftime('%Y%m%d-%H%M%S')}-{slugify(name)}-{uuid.uuid4().hex[:6]}"
    run_dir = RUNS_DIR / run_id
    run_dir.mkdir(parents=True)
    write_json_atomic(run_dir / "input.json", config)
    opt = meta.get("optimization") or {}
    status = {
        "id": run_id, "name": name, "description": config.get("description", ""), "state": "queued",
        "created": now(), "started": None, "finished": None, "exit_code": None, "error": None, "pid": None,
        "method": opt.get("method"), "volume_fraction": opt.get("volume_fraction"),
        "max_iterations": opt.get("max_iterations"), "num_voxels": (meta.get("grid") or {}).get("num_voxels"),
        "resolution": (meta.get("grid") or {}).get("resolution"), "threads": threads,
        "paper_reference": config.get("paper_reference"), "warnings": res.get("warnings", []),
    }
    write_json_atomic(run_dir / "status.json", status)
    jobs.submit(run_id)
    return {"id": run_id, "status": status}


def run_summary(run_dir: Path) -> dict | None:
    st = read_json(run_dir / "status.json")
    if not isinstance(st, dict):
        return None
    last = last_history_record(run_dir) or {}
    for k in ("iteration", "compliance", "energy", "volume_fraction", "elapsed"):
        st[k] = last.get(k)
    if st.get("state") == "queued":
        st["queue_position"] = jobs.queue_position(st.get("id", run_dir.name))
    return st


@app.get("/api/runs")
def list_runs() -> list[dict]:
    out = []
    for d in RUNS_DIR.iterdir():
        if d.is_dir() and RUN_ID_RE.match(d.name):
            s = run_summary(d)
            if s:
                out.append(s)
    out.sort(key=lambda s: s.get("created") or 0, reverse=True)
    return out


@app.get("/api/runs/{run_id}")
def get_run(run_id: str) -> dict:
    d = run_dir_of(run_id)
    return {"status": run_summary(d), "config": read_json(d / "config.json") or read_json(d / "input.json"),
            "meta": read_json(d / "meta.json"), "history": read_history(d), "result": read_json(d / "result.json")}


def sse(event: str, data: Any) -> str:
    return f"event: {event}\ndata: {json.dumps(data, separators=(',', ':'))}\n\n"


@app.get("/api/runs/{run_id}/events")
async def run_events(run_id: str, request: Request, since: int = -1) -> StreamingResponse:
    d = run_dir_of(run_id)

    async def stream():
        offset = 0
        last_status = None
        last_send = time.monotonic()
        meta_sent = False
        yield "retry: 2000\n\n"
        while True:
            if await request.is_disconnected():
                return
            if not (d / "status.json").is_file():  # the run was deleted
                yield sse("end", {"status": {"id": d.name, "state": "deleted"}, "result": None})
                return
            status = run_summary(d) or {}
            sent_any = False
            if not meta_sent and (d / "meta.json").is_file():
                meta = read_json(d / "meta.json")
                if meta is not None:
                    meta_sent = True
                    yield sse("meta", meta)
                    sent_any = True

            # new complete lines of history.jsonl
            try:
                with open(d / "history.jsonl", "rb") as f:
                    f.seek(offset)
                    chunk = f.read()
                end = chunk.rfind(b"\n")
                if end >= 0:
                    offset += end + 1
                    for line in chunk[: end + 1].splitlines():
                        line = line.strip()
                        if not line:
                            continue
                        try:
                            rec = json.loads(line)
                        except ValueError:
                            continue
                        if rec.get("iteration", -1) > since:
                            yield sse("iteration", rec)
                            sent_any = True
            except OSError:
                pass

            key = (status.get("state"), status.get("error"), status.get("queue_position"), status.get("finished"))
            if key != last_status:
                last_status = key
                yield sse("status", status)
                sent_any = True

            if status.get("state") in TERMINAL_STATES:
                result = read_json(d / "result.json")
                yield sse("end", {"status": status, "result": result})
                return

            if sent_any:
                last_send = time.monotonic()
            elif time.monotonic() - last_send > 15:
                last_send = time.monotonic()
                yield ": keep-alive\n\n"
            await asyncio.sleep(0.5)

    return StreamingResponse(stream(), media_type="text/event-stream",
                             headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


def frame_path(run_id: str, iteration: int, prefix: str, ext: str) -> Path:
    d = run_dir_of(run_id)
    if iteration < 0 or iteration > 1_000_000:
        raise HTTPException(status_code=404, detail="unknown iteration")
    p = d / "frames" / f"{prefix}{iteration:04d}{ext}"
    if not p.is_file():
        raise HTTPException(status_code=404, detail="frame not available")
    return p


IMMUTABLE = {"Cache-Control": "public, max-age=86400, immutable"}


@app.get("/api/runs/{run_id}/frames/{iteration}")
def get_frame(run_id: str, iteration: int) -> FileResponse:
    return FileResponse(frame_path(run_id, iteration, "frame_", ".json"), media_type="application/json",
                        headers=IMMUTABLE)


@app.get("/api/runs/{run_id}/theta/{iteration}")
def get_theta(run_id: str, iteration: int) -> FileResponse:
    return FileResponse(frame_path(run_id, iteration, "theta_", ".u8"), media_type="application/octet-stream",
                        headers=IMMUTABLE)


@app.get("/api/runs/{run_id}/log")
def get_log(run_id: str, tail: int = 200) -> dict:
    d = run_dir_of(run_id)
    return {"lines": tail_lines(d / "stdout.log", max(1, min(tail, 5000)))}


@app.get("/api/runs/{run_id}/files/{name}")
def get_file(run_id: str, name: str) -> FileResponse:
    d = run_dir_of(run_id)
    if name not in DOWNLOADABLE or not (d / name).is_file():
        raise HTTPException(status_code=404, detail="file not available")
    media = "application/json" if name.endswith(".json") else "text/plain"
    return FileResponse(d / name, media_type=media, filename=f"{run_id}_{name}")


@app.post("/api/runs/{run_id}/cancel")
def cancel_run(run_id: str) -> dict:
    run_dir_of(run_id)
    return {"id": run_id, "result": jobs.cancel(run_id)}


@app.delete("/api/runs/{run_id}")
def delete_run(run_id: str) -> dict:
    d = run_dir_of(run_id)
    st = read_json(d / "status.json") or {}
    if st.get("state") in ("queued", "running"):
        raise HTTPException(status_code=409, detail="cancel the run before deleting it")
    shutil.rmtree(d)
    return {"id": run_id, "deleted": True}


@app.get("/api/{rest:path}")
def api_not_found(rest: str) -> Response:
    raise HTTPException(status_code=404, detail="unknown endpoint")


if STATIC_DIR.is_dir():
    app.mount("/", StaticFiles(directory=STATIC_DIR, html=True), name="static")


def main() -> None:
    import uvicorn

    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", default=os.environ.get("MSD_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("MSD_PORT", "17860")))
    args = parser.parse_args()
    uvicorn.run(app, host=args.host, port=args.port, log_level="info", timeout_graceful_shutdown=2)


if __name__ == "__main__":
    main()
