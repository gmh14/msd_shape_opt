// "New run" panel: example picker, JSON editor (upload / download / format), quick controls that stay in sync
// with the JSON, Validate / Preview and Run.

import { api } from './api.js';
import {
  $, $$, el, icon, formatJSON, parseJSON, debounce, storage, downloadText, fmtInt, fmtGrid, confirmDialog,
  pathOfMessage, locateJSONPath,
} from './util.js';

const DEFAULTS = { method: 'msd', volume_fraction: 0.2, max_iterations: 200, initial_radius: 4 };
const MAX_UPLOAD = 1 << 20; // the server accepts configurations up to 1 MiB

const normMethod = (m) => {
  const s = String(m ?? '').toLowerCase();
  return s === 'ma' || s === 'medial_axis' || s === '1' ? 'ma' : 'msd';
};

export class NewRunPanel {
  constructor({ onPreview, onCreated, onEdited, onInvalid }) {
    this.h = { onPreview, onCreated, onEdited, onInvalid };
    this.section = $('#newrun-section');
    this.sel = $('#nr-example');
    this.desc = $('#nr-example-desc');
    this.methods = $$('input[name="nr-method"]');
    this.vf = $('#nr-vf');
    this.iters = $('#nr-iters');
    this.radius = $('#nr-radius');
    this.threads = $('#nr-threads');
    this.threadsHint = $('#nr-threads-hint');
    this.editor = $('#nr-editor');
    this.text = $('#nr-json');
    this.status = $('#nr-json-status');
    this.summary = $('#nr-json-summary');
    this.result = $('#nr-result');
    this.btnValidate = $('#nr-validate');
    this.btnRun = $('#nr-run');
    this.examples = [];
    this.cpu = 1;
    this.lastLoaded = null;
    this.binaryOk = true;
    this.quick = []; // numeric quick controls: {input, key, label, apply (debounced, flushable)}
    this.errorPath = null; // JSON path of the last server-side validation error (for "Show in editor")
    this.bind();
  }

  // ------------------------------------------------------------------------------------- setup

  bind() {
    this.sel.addEventListener('change', () => this.loadExample(this.sel.value));
    this.desc.addEventListener('click', () => this.desc.classList.toggle('is-open'));
    const reparse = debounce(() => this.onText(), 180);
    const save = debounce(() => storage.set('msd.draft', this.text.value), 600);
    this.text.addEventListener('input', () => { reparse(); save(); this.h.onEdited?.(); });
    window.addEventListener('pagehide', () => save.flush()); // keep the last keystrokes of the draft on reload
    this.text.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) { e.preventDefault(); this.validate(); }
    });

    for (const r of this.methods) r.addEventListener('change', () => { if (r.checked) this.setOpt('method', r.value); });
    // The numeric fields write into the JSON after a short typing pause. The pending edit is applied at once when
    // the field is left and before Validate / Run (flushQuick), so an action never sends an older value than the
    // field shows.
    const numeric = (input, key, label, { integer = false, min, max, minExclusive = false }) => {
      const range = `${minExclusive ? '(' : '['}${min}, ${max}]`;
      const apply = debounce(() => {
        const raw = input.value.trim();
        if (raw === '') { this.fieldError(input, null); this.setOpt(key, undefined); return; }
        const v = Number(raw);
        const bad = !Number.isFinite(v) || (integer && !Number.isInteger(v)) || (minExclusive ? v <= min : v < min) || v > max;
        if (bad) {
          this.fieldError(input, integer ? `Integer ${min}–${max}` : `Number in ${range}`, `enter ${integer ? 'an integer' : 'a number'} in ${range}`);
          return;
        }
        this.fieldError(input, null);
        this.setOpt(key, v);
      }, 350);
      input.addEventListener('input', apply);
      input.addEventListener('change', () => apply.flush());
      input.addEventListener('blur', () => apply.flush());
      this.quick.push({ input, key, label, apply });
    };
    numeric(this.vf, 'volume_fraction', 'Volume fraction', { min: 0, max: 1, minExclusive: true });
    numeric(this.iters, 'max_iterations', 'Max iterations', { integer: true, min: 1, max: 100000 });
    numeric(this.radius, 'initial_radius', 'Initial radius', { min: 0, max: 100, minExclusive: true });
    this.threads.addEventListener('input', () => {
      const t = this.readThreads(false);
      if (t != null) storage.set('msd.threads', String(t));
    });

    $('#nr-upload').addEventListener('change', (e) => this.upload(e.target));
    $('#nr-download').addEventListener('click', () => this.download());
    $('#nr-format').addEventListener('click', () => {
      const p = parseJSON(this.text.value);
      if (!p.ok) { this.onText(); this.gotoError(p); return; }
      this.setText(formatJSON(p.value));
    });
    this.btnValidate.addEventListener('click', () => this.validate());
    this.btnRun.addEventListener('click', () => this.run());
  }

  async init(health) {
    this.cpu = health?.cpu_count || 1;
    this.binaryOk = health ? health.binary_exists !== false : true;
    this.threads.max = String(this.cpu);
    this.threadsHint.textContent = `0 = all ${this.cpu} cores`;
    const stored = storage.get('msd.threads');
    const t = stored != null ? Number(stored) : (health?.default_threads ?? 0);
    this.threads.value = String(Number.isInteger(t) && t >= 0 && t <= this.cpu ? t : 0);
    try {
      this.examples = await api.examples();
    } catch (e) {
      this.examples = [];
      this.sel.replaceChildren(el('option', { value: '' }, 'Examples unavailable'));
      this.showResult('error', `Could not load the examples: ${e.message}`);
    }
    if (this.examples.length) this.renderExamples();

    const draft = storage.get('msd.draft');
    const exampleId = storage.get('msd.example');
    if (draft && draft.trim()) {
      this.setText(draft, { silent: true });
      if (exampleId && this.examples.some((x) => x.id === exampleId)) this.sel.value = exampleId;
      this.renderExampleDesc(this.sel.value);
      // remember the example's own text so that switching examples only asks when there are real edits
      if (this.sel.value) {
        api.example(this.sel.value).then((cfg) => { this.lastLoaded = formatJSON(cfg); }).catch(() => {});
      }
    } else if (this.examples.length) {
      const first = this.examples.find((x) => x.id === 'cantilever_truss') || this.examples[0];
      this.sel.value = first.id;
      await this.loadExample(first.id, { force: true });
    }
  }

  renderExamples() {
    const paper = this.examples.filter((x) => x.paper_reference);
    const other = this.examples.filter((x) => !x.paper_reference);
    const opt = (x) => el('option', { value: x.id }, `${x.name}  (${fmtGrid(x.resolution)})`);
    const groups = [el('option', { value: '', disabled: true, hidden: true }, 'Custom configuration')];
    if (other.length) groups.push(el('optgroup', { label: 'Examples' }, other.map(opt)));
    if (paper.length) groups.push(el('optgroup', { label: 'Paper experiments' }, paper.map(opt)));
    this.sel.replaceChildren(...groups);
  }

  renderExampleDesc(id) {
    const x = this.examples.find((e) => e.id === id);
    if (!x) { this.desc.replaceChildren(); this.desc.hidden = true; return; }
    this.desc.hidden = false;
    this.desc.title = x.description || '';
    const tags = [];
    if (x.paper_reference) {
      const pr = x.paper_reference;
      tags.push(el('span', { class: 'tag' }, [pr.figure, pr.column ? `col. ${pr.column}` : null].filter(Boolean).join(' ')));
    }
    tags.push(el('span', { class: 'tag' }, `${fmtInt(x.num_voxels)} voxels`));
    this.desc.replaceChildren(...tags, x.description || '');
  }

  async loadExample(id, { force = false } = {}) {
    if (!id) return;
    if (!force && this.isDirty()) {
      const ok = await confirmDialog({
        title: 'Replace the configuration?',
        text: 'The editor contains changes that are not part of an example. Loading the example replaces them (use Download JSON first to keep them).',
        ok: 'Replace',
      });
      if (!ok) { this.sel.value = storage.get('msd.example') || ''; return; }
    }
    try {
      const cfg = await api.example(id);
      const text = formatJSON(cfg);
      this.setText(text);
      this.lastLoaded = text;
      storage.set('msd.example', id);
      storage.set('msd.draft', text);
      this.renderExampleDesc(id);
      this.clearResult();
    } catch (e) {
      this.showResult('error', `Could not load the example: ${e.message}`);
    }
  }

  /** True when the editor holds edits that exist nowhere else (an example, an uploaded file, a run). */
  isDirty() {
    const t = this.text.value.trim();
    return t !== '' && (this.lastLoaded == null || t !== this.lastLoaded.trim());
  }

  /** Puts a configuration (e.g. of an existing run) into the editor and reveals the panel. */
  setConfig(cfg, label) {
    this.setText(formatJSON(cfg));
    this.lastLoaded = this.text.value;
    this.sel.value = '';
    this.renderExampleDesc('');
    storage.set('msd.draft', this.text.value);
    storage.set('msd.example', null);
    this.showResult('info', label || 'Configuration loaded into the editor.', { reveal: false });
    this.section.open = true;
    this.editor.open = true;
    this.section.scrollIntoView({ behavior: 'smooth', block: 'start' });
    this.text.focus({ preventScroll: true });
  }

  // ------------------------------------------------------------------------------------- editor

  setText(text, { silent = false } = {}) {
    const top = this.text.scrollTop;
    this.text.value = text;
    this.text.scrollTop = top;
    this.onText();
    if (!silent) this.h.onEdited?.();
  }

  onText() {
    const p = parseJSON(this.text.value);
    this.parsed = p;
    this.status.className = 'editor-status';
    if (!p.ok) {
      this.status.classList.add('is-error');
      const where = p.line != null ? `Line ${p.line}, column ${p.column}: ` : '';
      const kids = [icon('alert'), el('span', {}, `${where}${p.message}`)];
      if (p.position != null) {
        const go = el('button', { type: 'button' }, 'Go to error');
        go.addEventListener('click', () => this.gotoError(p));
        kids.push(go);
      }
      this.status.replaceChildren(...kids);
      this.summary.textContent = '';
      this.syncQuick(null);
      return;
    }
    const cfg = p.value;
    if (!cfg || typeof cfg !== 'object' || Array.isArray(cfg)) {
      this.status.classList.add('is-error');
      this.status.replaceChildren(icon('alert'), el('span', {}, 'The configuration must be a JSON object { ... }.'));
      this.syncQuick(null);
      return;
    }
    this.status.classList.add('is-ok');
    const res = cfg.grid && Array.isArray(cfg.grid.resolution) ? cfg.grid.resolution : null;
    const nvox = res && res.length === 3 ? res.reduce((a, b) => a * Number(b), 1) : null;
    const sk = cfg.skeleton || {};
    // only the syntax is checked here; the problem itself is checked by the server (Validate / Preview)
    const parts = ['JSON syntax OK'];
    if (res) parts.push(`${fmtGrid(res)} grid (${fmtInt(nvox)} voxels)`);
    if (Array.isArray(sk.vertices)) parts.push(`${sk.vertices.length} vertices`);
    if (Array.isArray(sk.edges)) parts.push(`${sk.edges.length} edges`);
    const text = el('span', {}, parts.join(' · '));
    if (nvox > 60000) text.append(el('span', { class: 'muted' }, ' · large grid: every iteration takes a while'));
    this.status.replaceChildren(icon('check'), text);
    this.summary.textContent = cfg.name ? String(cfg.name) : '';
    this.syncQuick(cfg);
  }

  gotoError(p) {
    if (p.position == null) return;
    this.selectInEditor(p.position, Math.min(this.text.value.length, p.position + 1));
  }

  /** Opens the editor, selects text[start, end) and scrolls it into view (the textarea does not wrap lines). */
  selectInEditor(start, end) {
    const t = this.text;
    this.editor.open = true;
    t.focus({ preventScroll: true });
    t.setSelectionRange(start, end);
    const before = t.value.slice(0, start);
    const line = before.split('\n').length;
    const column = start - (before.lastIndexOf('\n') + 1);
    const cs = getComputedStyle(t);
    const lineHeight = parseFloat(cs.lineHeight) || 18;
    const charWidth = (parseFloat(cs.fontSize) || 12) * 0.6; // monospace advance
    t.scrollTop = Math.max(0, (line - 4) * lineHeight);
    t.scrollLeft = Math.max(0, column * charWidth - t.clientWidth / 3);
    t.scrollIntoView({ block: 'nearest', behavior: 'smooth' });
  }

  /** Selects the configuration entry named by the last server-side error (e.g. skeleton.edges[10]). */
  showErrorInEditor() {
    const loc = this.errorPath ? locateJSONPath(this.text.value, this.errorPath) : null;
    if (!loc || loc.depth === 0) return false;
    this.section.open = true;
    this.selectInEditor(loc.start, loc.end);
    return true;
  }

  /** Copies optimization.* of the JSON into the quick controls (except the one being edited). */
  syncQuick(cfg) {
    const o = (cfg && cfg.optimization && typeof cfg.optimization === 'object') ? cfg.optimization : {};
    const method = normMethod(o.method ?? DEFAULTS.method);
    for (const r of this.methods) r.checked = r.value === method;
    const put = (input, v) => {
      if (document.activeElement === input) return;
      input.value = v == null ? '' : String(v);
      this.fieldError(input, null);
    };
    put(this.vf, o.volume_fraction);
    put(this.iters, o.max_iterations);
    put(this.radius, o.initial_radius);
    const disabled = !cfg;
    for (const x of [...this.methods, this.vf, this.iters, this.radius]) x.disabled = disabled;
  }

  setOpt(key, value) {
    const p = parseJSON(this.text.value);
    if (!p.ok || !p.value || typeof p.value !== 'object' || Array.isArray(p.value)) return;
    const cfg = p.value;
    if (!cfg.optimization || typeof cfg.optimization !== 'object') cfg.optimization = {};
    if (value === undefined) delete cfg.optimization[key];
    else cfg.optimization[key] = value;
    this.setText(formatJSON(cfg));
    storage.set('msd.draft', this.text.value);
  }

  /** Marks a field as invalid with a short hint below it (null clears it); `full` is the complete sentence. */
  fieldError(input, hint, full = hint) {
    input.classList.toggle('is-invalid', !!hint);
    input.setAttribute('aria-invalid', hint ? 'true' : 'false');
    let box = input.parentElement.querySelector('.field-error');
    if (!hint) {
      if (box) box.remove(); // the hint is out of the layout flow (app.css): adding or removing it moves nothing
      delete input.dataset.error;
      input.removeAttribute('aria-describedby');
      return;
    }
    if (!box) { box = el('span', { class: 'field-error', id: `${input.id}-error` }); input.after(box); }
    box.textContent = hint;
    box.title = `${full[0].toUpperCase()}${full.slice(1)}`;
    input.dataset.error = full;
    input.setAttribute('aria-describedby', box.id);
  }

  /** Applies quick-control edits that still wait for the typing pause. Returns the first invalid control or null. */
  flushQuick() {
    for (const q of this.quick) q.apply.flush();
    return this.quick.find((q) => !q.input.disabled && q.input.getAttribute('aria-invalid') === 'true') || null;
  }

  /** False (with a message, focus on the field) when a quick control holds a value that is not in the JSON. */
  quickControlsOk(action) {
    const bad = this.flushQuick();
    if (!bad) return true;
    this.showResult('error', `${bad.label}: ${bad.input.dataset.error || 'invalid value'}. Nothing was ${action}.`,
      { title: 'Check the quick settings', reveal: false });
    bad.input.focus();
    return false;
  }

  readThreads(report = true) {
    const raw = this.threads.value.trim();
    const v = raw === '' ? 0 : Number(raw);
    if (!Number.isInteger(v) || v < 0 || v > this.cpu) {
      this.fieldError(this.threads, `Integer 0–${this.cpu}`, `enter an integer in [0, ${this.cpu}] (0 = all cores)`);
      if (report) this.threads.focus();
      return null;
    }
    this.fieldError(this.threads, null);
    return v;
  }

  async upload(input) {
    const file = input.files && input.files[0];
    input.value = '';
    if (!file) return;
    if (file.size > MAX_UPLOAD) {
      this.showResult('error', `${file.name} is ${fmtInt(file.size)} bytes; the server accepts configurations up to 1 MiB.`);
      return;
    }
    let text;
    try { text = await file.text(); } catch (e) { this.showResult('error', `Could not read ${file.name}: ${e.message}`); return; }
    const p = parseJSON(text);
    this.setText(p.ok ? formatJSON(p.value) : text);
    this.lastLoaded = this.text.value;
    this.sel.value = '';
    this.renderExampleDesc('');
    storage.set('msd.example', null);
    storage.set('msd.draft', this.text.value);
    this.editor.open = true;
    if (p.ok) this.showResult('info', `Loaded ${file.name}. Validate / Preview to check it.`);
    else { this.showResult('error', `${file.name} is not valid JSON: ${p.message}${p.line != null ? ` (line ${p.line}, column ${p.column})` : ''}`); }
  }

  download() {
    const p = parseJSON(this.text.value);
    const name = p.ok && p.value && typeof p.value.name === 'string' && p.value.name.trim() ? p.value.name.trim() : 'config';
    downloadText(this.text.value, `${name.replace(/[^A-Za-z0-9_.-]+/g, '_')}.json`);
  }

  // ------------------------------------------------------------------------------------- actions

  configOrError() {
    const p = parseJSON(this.text.value);
    if (!p.ok) {
      this.onText();
      // gotoError() scrolls the editor (and with it this message just below it) into view
      this.showResult('error', `The JSON is not valid${p.line != null ? ` (line ${p.line}, column ${p.column})` : ''}: ${p.message}`,
        { reveal: p.position == null });
      this.gotoError(p);
      return null;
    }
    if (!p.value || typeof p.value !== 'object' || Array.isArray(p.value)) {
      this.showResult('error', 'The configuration must be a JSON object { ... }.');
      return null;
    }
    return p.value;
  }

  /** A configuration the server rejected: show the reason here and let the main area flag a stale preview. */
  invalid(message, title = 'Invalid configuration') {
    const path = pathOfMessage(message);
    this.showResult('error', message || 'invalid configuration', { title, path });
    const loc = path ? locateJSONPath(this.text.value, path) : null;
    this.h.onInvalid?.(message || 'invalid configuration', !!(loc && loc.depth > 0));
  }

  async validate() {
    if (this.btnValidate.disabled) return;
    if (!this.quickControlsOk('validated')) return;
    const cfg = this.configOrError();
    if (!cfg) return;
    this.busy(this.btnValidate, true);
    try {
      const res = await api.validate(cfg);
      if (res.ok) {
        const warnings = res.warnings || [];
        this.showResult(warnings.length ? 'warning' : 'ok', null, { meta: res.meta, warnings });
        this.h.onPreview?.({ meta: res.meta, config: cfg, warnings });
      } else {
        this.invalid(res.error);
      }
    } catch (e) {
      this.showResult('error', e.message, { title: 'Validation failed' });
    } finally {
      this.busy(this.btnValidate, false);
    }
  }

  async run() {
    if (this.btnRun.disabled) return;
    if (!this.quickControlsOk('started')) return;
    const cfg = this.configOrError();
    if (!cfg) return;
    const threads = this.readThreads();
    if (threads == null) {
      this.showResult('error', `Threads: enter an integer in [0, ${this.cpu}] (0 = all cores). Nothing was started.`,
        { title: 'Check the quick settings', reveal: false });
      return;
    }
    if (!this.binaryOk) { this.showResult('error', 'The optimizer binary is missing on the server; build the project first.'); return; }
    this.busy(this.btnRun, true);
    try {
      // validate first: api/validate reports problems with HTTP 200, so an invalid configuration does not show
      // up as a failed request (the server validates again when it creates the run)
      const check = await api.validate(cfg);
      if (!check.ok) {
        this.invalid(check.error);
        return;
      }
      const res = await api.createRun(cfg, threads);
      const w = (res.status && res.status.warnings) || [];
      // no scrolling to this note: the new run opens in the main area (which repeats any warning) and the sidebar
      // goes back to the run list, where the new run is highlighted at the top
      this.showResult(w.length ? 'warning' : 'ok', `Started run ${res.id}.`, { warnings: w, reveal: false });
      this.h.onCreated?.(res);
    } catch (e) {
      if (e.status === 400) this.invalid(e.message, 'The server rejected the configuration');
      else this.showResult('error', e.message, { title: 'Could not start the run' });
    } finally {
      this.busy(this.btnRun, false);
    }
  }

  busy(btn, on) {
    btn.classList.toggle('is-busy', on);
    btn.disabled = on;
    btn.setAttribute('aria-busy', on ? 'true' : 'false');
  }

  clearResult() { this.result.hidden = true; this.result.replaceChildren(); this.errorPath = null; }

  /**
   * kind: ok | warning | error | info. `path`: the configuration entry an error is about (adds "Show in editor").
   * The box sits below the buttons, which can be the bottom of the sidebar: it is scrolled into view (`reveal`).
   */
  showResult(kind, message, { title, meta, warnings = [], path = null, reveal = true } = {}) {
    const box = this.result;
    box.className = `result is-${kind}`;
    box.hidden = false;
    box.setAttribute('role', kind === 'error' ? 'alert' : 'status');
    this.errorPath = kind === 'error' ? path : null;
    const body = el('div', { class: 'msg' });
    if (kind === 'error') {
      body.append(el('strong', {}, title || 'Error'), el('span', { class: 'err-text' }, message));
      const loc = path ? locateJSONPath(this.text.value, path) : null;
      if (loc && loc.depth > 0) {
        const go = el('button', { class: 'btn btn-xs', type: 'button' }, icon('edit'), 'Show in editor');
        go.addEventListener('click', () => this.showErrorInEditor());
        body.append(el('div', { class: 'alert-actions' }, go));
      }
    } else {
      if (meta) {
        const g = meta.grid || {};
        const bc = meta.boundary_conditions || {};
        const sk = meta.skeleton || {};
        body.append(el('strong', {}, warnings.length ? 'Valid, with warnings' : 'Valid configuration'),
          el('div', {}, `${fmtGrid(g.resolution)} voxels · ${(sk.vertices || []).length} vertices · ${(sk.edges || []).length} edges · ${fmtInt((bc.fixed_vertices || []).length)} fixed and ${fmtInt((bc.loads || []).length)} loaded grid vertices. The preview is shown in the 3D view.`));
      } else if (message) {
        body.append(el('div', {}, message));
      }
      if (warnings.length) body.append(el('ul', {}, warnings.map((w) => el('li', {}, w))));
    }
    const ic = { ok: 'check', warning: 'alert', error: 'alert', info: 'info' }[kind] || 'info';
    box.replaceChildren(icon(ic), body);
    // On wide screens the sidebar is its own scroll container: scrolling it never moves the main area. In the
    // single-column layout only errors are brought into view (a valid preview scrolls to the 3D view instead).
    const sidebar = this.section.closest('.sidebar');
    const ownScroller = sidebar && /(auto|scroll)/.test(getComputedStyle(sidebar).overflowY);
    if (reveal && (ownScroller || kind === 'error')) this.revealResult(ownScroller ? sidebar : null);
  }

  /**
   * Scrolls the result box into view, and checks again once the layout has settled: the run list above this panel
   * can still grow while the page loads (or a run starts), which moves the box and can stop a smooth scroll
   * half-way. A user who scrolls in the meantime is not overridden. scroller: the sidebar, or null for the page.
   */
  revealResult(scroller) {
    const box = this.result;
    const token = (this.revealToken = (this.revealToken || 0) + 1);
    const visible = () => {
      const r = box.getBoundingClientRect();
      let top = 0;
      let bottom = window.innerHeight;
      if (scroller) {
        const s = scroller.getBoundingClientRect();
        top = Math.max(top, s.top);
        bottom = Math.min(bottom, s.bottom);
      }
      return r.top >= top - 1 && r.bottom <= bottom + 1;
    };
    let userMoved = false;
    const ac = new AbortController();
    for (const t of ['wheel', 'touchstart', 'keydown']) {
      (scroller || window).addEventListener(t, () => { userMoved = true; }, { passive: true, signal: ac.signal });
    }
    requestAnimationFrame(() => { if (!box.hidden) box.scrollIntoView({ block: 'nearest', behavior: 'smooth' }); });
    for (const ms of [600, 1400]) {
      setTimeout(() => {
        if (ms === 1400) ac.abort();
        if (token !== this.revealToken || box.hidden || userMoved || visible()) return;
        box.scrollIntoView({ block: 'nearest', behavior: 'auto' });
      }, ms);
    }
  }
}
