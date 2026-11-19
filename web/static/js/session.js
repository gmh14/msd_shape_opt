// One open run: its status, meta, history, the live event stream and a small cache of frames.
//
// The history is a Map iteration -> record, so iterations that the server re-sends after a reconnect are
// de-duplicated. The stream is reopened by hand (not by EventSource's own retry) so that the `since` parameter
// always reflects the newest iteration we already have.

import { api, isTerminal } from './api.js';

const parse = (s) => { try { return JSON.parse(s); } catch { return null; } };

export class FrameCache {
  constructor(runId, maxBytes = 96 * 1024 * 1024) {
    this.runId = runId;
    this.map = new Map(); // iteration -> {theta, positions, vertices, missing}; insertion order = LRU order
    this.inflight = new Map();
    this.bytes = 0;
    this.maxBytes = maxBytes;
    this.ctrl = new AbortController();
  }

  has(k) { return this.map.has(k); }

  /** Loads theta + frame JSON of iteration k (both in parallel). Missing files (404) give null fields. */
  get(k) {
    const hit = this.map.get(k);
    if (hit) {
      this.map.delete(k);
      this.map.set(k, hit);
      return Promise.resolve(hit);
    }
    if (this.inflight.has(k)) return this.inflight.get(k);
    const signal = this.ctrl.signal;
    const missing = (e) => { if (e && e.status === 404) return null; throw e; };
    const p = Promise.all([
      api.theta(this.runId, k, signal).catch(missing),
      api.frame(this.runId, k, signal).catch(missing),
    ]).then(([buf, fr]) => {
      const entry = {
        theta: buf ? new Uint8Array(buf) : null,
        positions: fr && Array.isArray(fr.primitive_positions) ? Float32Array.from(fr.primitive_positions) : null,
        vertices: fr && Array.isArray(fr.vertices) ? fr.vertices : null,
      };
      entry.missing = !entry.theta && !entry.positions;
      entry.bytes = (entry.theta ? entry.theta.byteLength : 0) + (entry.positions ? entry.positions.byteLength : 0) + 256;
      this.inflight.delete(k);
      this.map.set(k, entry);
      this.bytes += entry.bytes;
      while (this.bytes > this.maxBytes && this.map.size > 1) {
        const [oldK, old] = this.map.entries().next().value;
        this.map.delete(oldK);
        this.bytes -= old.bytes;
      }
      return entry;
    }, (e) => {
      this.inflight.delete(k);
      throw e;
    });
    this.inflight.set(k, p);
    return p;
  }

  prefetch(list) {
    for (const k of list) if (!this.map.has(k) && !this.inflight.has(k)) this.get(k).catch(() => {});
  }

  close() { this.ctrl.abort(); this.map.clear(); this.inflight.clear(); }
}

export class RunSession {
  /** handlers: onLoad, onMeta, onIterations(records), onStatus, onEnd, onConnection(state), onGone, onError */
  constructor(id, handlers) {
    this.id = id;
    this.h = handlers;
    this.history = new Map();
    this.status = null;
    this.meta = null;
    this.config = null;
    this.result = null;
    this.es = null;
    this.closed = false;
    this.retry = 0;
    this.timer = null;
    this.pending = [];
    this.flushQueued = false;
    this.frames = new FrameCache(id);
    this.ctrl = new AbortController();
  }

  get state() { return this.status ? this.status.state : null; }

  lastIteration() {
    let m = -1;
    for (const k of this.history.keys()) if (k > m) m = k;
    return m;
  }

  /** Sorted records. */
  records() { return [...this.history.values()].sort((a, b) => a.iteration - b.iteration); }

  async load() {
    const d = await api.run(this.id, this.ctrl.signal);
    if (this.closed) return;
    this.status = d.status;
    this.config = d.config;
    this.meta = d.meta;
    this.result = d.result;
    for (const r of d.history || []) if (Number.isInteger(r.iteration)) this.history.set(r.iteration, r);
    this.h.onLoad?.(this);
    if (!isTerminal(this.state)) this.connect();
  }

  connect() {
    if (this.closed) return;
    const es = new EventSource(api.eventsUrl(this.id, this.lastIteration()));
    this.es = es;
    es.addEventListener('open', () => { this.retry = 0; this.h.onConnection?.('live'); });
    es.addEventListener('meta', (e) => {
      const m = parse(e.data);
      if (m && !this.closed) { this.meta = m; this.h.onMeta?.(m); }
    });
    es.addEventListener('iteration', (e) => {
      const r = parse(e.data);
      if (!r || !Number.isInteger(r.iteration) || this.history.has(r.iteration)) return; // de-duplicate
      this.history.set(r.iteration, r);
      this.pending.push(r);
      // iterations can arrive in bursts (e.g. after a reconnect): hand them over once per animation frame
      if (!this.flushQueued) {
        this.flushQueued = true;
        requestAnimationFrame(() => this.flushPending());
      }
    });
    es.addEventListener('status', (e) => {
      const s = parse(e.data);
      if (s && !this.closed) { this.status = s; this.h.onStatus?.(s); }
    });
    es.addEventListener('end', (e) => {
      const d = parse(e.data) || {};
      es.close();
      this.es = null;
      if (this.closed) return;
      if (d.status && d.status.state === 'deleted') { this.h.onGone?.(); return; } // removed while open
      this.flushPending(); // the last iterations may have arrived together with the end event
      if (d.status) this.status = d.status;
      if (d.result !== undefined) this.result = d.result;
      this.h.onConnection?.('closed');
      this.finish();
    });
    es.onerror = () => {
      if (this.closed || this.es !== es) return;
      es.close();
      this.es = null;
      this.h.onConnection?.('reconnecting');
      this.scheduleReconnect();
    };
  }

  flushPending() {
    this.flushQueued = false;
    const batch = this.pending;
    this.pending = [];
    if (!this.closed && batch.length) this.h.onIterations?.(batch);
  }

  async scheduleReconnect() {
    clearTimeout(this.timer);
    const delay = Math.min(15000, 1000 * 2 ** this.retry++);
    this.timer = setTimeout(async () => {
      if (this.closed) return;
      // a 404 here means the run was deleted (EventSource itself cannot tell us why it failed)
      try {
        await api.log(this.id, 1, this.ctrl.signal);
      } catch (e) {
        if (this.closed) return;
        if (e.status === 404) { this.h.onGone?.(); return; }
        this.scheduleReconnect();
        return;
      }
      this.connect();
    }, delay);
  }

  /** After the terminal event: reload everything once, so that nothing missed by the stream is lost. */
  async finish() {
    try {
      const d = await api.run(this.id, this.ctrl.signal);
      if (this.closed) return;
      this.status = d.status;
      this.meta = d.meta || this.meta;
      this.config = d.config || this.config;
      this.result = d.result;
      const fresh = [];
      for (const r of d.history || []) {
        if (Number.isInteger(r.iteration) && !this.history.has(r.iteration)) { this.history.set(r.iteration, r); fresh.push(r); }
      }
      if (fresh.length) this.h.onIterations?.(fresh);
    } catch (e) {
      if (this.closed || e.name === 'AbortError') return;
      this.h.onError?.(e);
    }
    if (!this.closed) this.h.onEnd?.(this);
  }

  close() {
    this.closed = true;
    clearTimeout(this.timer);
    if (this.es) this.es.close();
    this.es = null;
    this.ctrl.abort();
    this.frames.close();
  }
}
