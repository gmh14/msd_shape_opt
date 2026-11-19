// The main area: a problem preview (from api/validate) or an open run (live or finished).
//
// Display pipeline of a run: showIteration(k) updates the metrics, the chart markers and the playback controls
// immediately and asks for the frame of k. Frames are loaded one at a time; if newer iterations were requested
// while a frame was loading (fast live updates, fast scrubbing), the intermediate ones are skipped and only the
// newest is loaded and drawn.

import { Viewer } from './viewer.js';
import { ChartGroup, LineChart } from './charts.js';
import { RunSession } from './session.js';
import { api, isTerminal } from './api.js';
import { stateChip, methodLabel, methodLong } from './status.js';
import {
  $, $$, el, icon, clamp, isNum, fmtNum, fmtInt, fmtSci, fmtDuration, fmtWhen, fmtWhenFull, fmtGrid, fmtSigned,
  toast, downloadBlob, storage,
} from './util.js';

const CONVERGENCE = 1e-5;
const FILES = [
  ['final_voxels.obj', 'Voxel shape (θ > 0.5), as written by the original code'],
  ['final_primitives.obj', 'Union of the generalized enveloping primitives'],
  ['final_skeleton.obj', 'Optimized skeletal graph (vertices and edges)'],
  ['history.jsonl', 'One JSON record per iteration'],
  ['energy.txt', 'Energy log of the original optimizer'],
  ['config.json', 'Normalized configuration (defaults filled in)'],
  ['stdout.log', 'Console output of the optimizer'],
];
const sciShort = (v) => (isNum(v) ? (v === 0 ? '0' : v.toExponential(2)) : '–');

// ---- keyboard shortcuts (Space = play / pause, Left / Right = step, Home / End = first / last iteration)
const SHORTCUT_KEYS = new Set([' ', 'ArrowLeft', 'ArrowRight', 'Home', 'End']);
const NON_TEXT_INPUTS = new Set(['checkbox', 'radio', 'range', 'button', 'submit', 'reset', 'file', 'color', 'image']);
const isTextEntry = (t) => t.isContentEditable || t.tagName === 'TEXTAREA' || (t.tagName === 'INPUT' && !NON_TEXT_INPUTS.has(t.type));

/** Whether the key has its own meaning on a focused control (Space presses a button, arrows move a slider, ...). */
function hasNativeKey(t, key) {
  const tag = t.tagName;
  const type = tag === 'INPUT' ? t.type : '';
  if (tag === 'SELECT') return true;
  if (key === ' ') return tag === 'BUTTON' || tag === 'SUMMARY' || tag === 'TR' || type === 'checkbox' || type === 'radio' || type === 'file';
  return type === 'range' || type === 'radio';
}

export class Workspace {
  constructor(handlers) {
    this.h = handlers;
    this.root = $('#workspace');
    this.emptyState = $('#empty-state');
    this.mode = 'empty';
    this.session = null;
    this.preview = null;
    this.iters = [];
    this.cur = null;
    this.follow = false;
    this.playing = false;
    this.speed = 4;
    this.wantFrame = null;
    this.shownFrame = null;
    this.loadingFrame = false;
    this.frameInfo = null;
    this.logLines = [];
    this.provisional = false;

    const ids = ['ws-title', 'ws-eyebrow', 'ws-chip', 'ws-live', 'ws-sub', 'ws-alerts', 'ws-run', 'ws-cancel', 'ws-edit',
      'ws-delete', 'ws-close', 'viewer-caption', 'viewer-notice', 'viewer-legend', 'thr-range', 'thr-value', 'pb-first',
      'pb-prev', 'pb-play', 'pb-next', 'pb-last', 'pb-range', 'pb-label', 'pb-speed', 'pb-follow', 'playback', 'metrics',
      'problem', 'problem-kv', 'problem-warnings', 'charts', 'chart-empty', 'chart-stack', 'file-list', 'log-details',
      'log-pre', 'log-hide-events', 'log-refresh', 'history-details', 'history-wrap', 'vt-clip', 'viewer-shot',
      'm-compliance', 'm-compliance-sub', 'm-paper', 'm-volume', 'm-volume-target', 'm-volume-fill', 'm-volume-tick',
      'm-volume-meter', 'm-energy', 'm-penalty', 'm-iter', 'm-elapsed', 'm-change', 'm-conv', 'm-filled', 'm-radius',
      'm-step', 'm-ls'];
    this.e = {};
    for (const id of ids) this.e[id.replace(/-([a-z])/g, (_, c) => c.toUpperCase())] = document.getElementById(id);
    this.bottom = $('.ws-bottom');

    this.viewer = new Viewer($('#viewer'), { gizmo: $('#viewer-gizmo') });
    if (!this.viewer.available) {
      this.webglMissing = true;
      this.setNotice('The 3D view is unavailable: WebGL could not be initialized in this browser. Metrics, charts and downloads still work.');
    }
    this.viewer.onInfo = () => this.updateCaption();
    this.viewer.onContextLost = () => this.setNotice('The 3D view lost its GPU context; waiting for the browser to restore it…');
    this.viewer.onContextRestored = () => this.setNotice(null);

    this.restorePrefs();
    this.initCharts();
    this.bind();
  }

  // ===================================================================================== setup

  restorePrefs() {
    let prefs = {};
    try { prefs = JSON.parse(storage.get('msd.layers', '{}')) || {}; } catch { prefs = {}; }
    for (const cb of $$('.viewer-toolbar input[data-layer]')) {
      const k = cb.dataset.layer;
      if (typeof prefs[k] === 'boolean') cb.checked = prefs[k];
      if (this.viewer.available) this.viewer.layers[k] = cb.checked;
    }
    if (typeof prefs.clip === 'boolean') this.e.vtClip.checked = prefs.clip;
    if (this.viewer.available) this.viewer.clip = this.e.vtClip.checked;
    this.updateLegend();
  }

  savePrefs() {
    const prefs = { clip: this.e.vtClip.checked };
    for (const cb of $$('.viewer-toolbar input[data-layer]')) prefs[cb.dataset.layer] = cb.checked;
    storage.set('msd.layers', JSON.stringify(prefs));
  }

  initCharts() {
    this.charts = new ChartGroup({ onPick: (k) => this.userShow(k) });
    const fmt4 = (v) => fmtNum(v, 4);
    this.charts.add(new LineChart($('#chart-energy'), {
      title: 'Compliance and total energy', height: 176, yScale: 'log', logToggle: true,
      series: [
        { key: 'compliance', label: 'Compliance', color: 'var(--series-1)', format: fmt4 },
        { key: 'energy', label: 'Total energy', color: 'var(--series-2)', format: fmt4 },
      ],
      extraRows: (r) => [{ label: 'volume penalty', value: fmtNum(r.volume_penalty, 3) }],
      ariaLabel: 'Line chart of the compliance and the total energy per iteration. The values are listed in the iteration table.',
    }));
    this.charts.add(new LineChart($('#chart-volume'), {
      title: 'Volume fraction', note: 'dashed: target', height: 120,
      series: [{ key: 'volume_fraction', label: 'volume fraction', color: 'var(--series-3)', format: (v) => v.toFixed(3) }],
      refs: () => (isNum(this.targetVf()) ? [{ value: this.targetVf(), label: `target ${this.targetVf()}` }] : []),
      extraRows: (r) => [
        { label: 'target', value: isNum(this.targetVf()) ? this.targetVf().toFixed(3) : '–', ref: true },
        { label: 'filled voxels', value: fmtInt(r.filled_voxels) },
      ],
      ariaLabel: 'Line chart of the volume fraction per iteration with the target as a reference line.',
    }));
    this.charts.add(new LineChart($('#chart-change'), {
      title: 'Max change', note: 'dashed: convergence threshold', height: 120, yScale: 'log',
      series: [{ key: 'max_change', label: 'max change', color: 'var(--series-4)', format: sciShort }],
      refs: () => [{ value: CONVERGENCE, label: 'converged below 10⁻⁵' }],
      extraRows: (r) => [{ label: 'step size', value: sciShort(r.step_size) }, { label: 'gradient norm', value: fmtNum(r.gradient_norm, 3) }],
      ariaLabel: 'Line chart of the convergence measure max change per iteration (log scale) with the threshold 1e-5.',
    }));
    const logCb = $('#chart-log');
    logCb.checked = storage.get('msd.chartLog', '1') === '1';
    this.charts.setLog(logCb.checked);
    logCb.addEventListener('change', () => { this.charts.setLog(logCb.checked); storage.set('msd.chartLog', logCb.checked ? '1' : '0'); });
  }

  bind() {
    const E = this.e;
    for (const cb of $$('.viewer-toolbar input[data-layer]')) {
      cb.addEventListener('change', () => {
        this.viewer.setLayer?.(cb.dataset.layer, cb.checked);
        if (cb.dataset.layer === 'primitives' && cb.checked && this.mode === 'run' && !this.loadingFrame && !this.viewer.hasPrimitives?.()) {
          this.setNotice(this.cur == null ? 'The primitives appear with the first iteration.' : `No primitive data for iteration ${this.cur}.`, 'corner', 3000);
        }
        this.savePrefs();
        this.updateLegend();
        this.updateCaption();
      });
    }
    E.vtClip.addEventListener('change', () => { this.viewer.setClip?.(E.vtClip.checked); this.savePrefs(); });
    E.thrRange.addEventListener('input', () => {
      const t = Number(E.thrRange.value);
      E.thrValue.textContent = t.toFixed(2);
      this.viewer.setThreshold?.(t);
    });
    for (const b of $$('.views [data-view]')) b.addEventListener('click', () => this.viewer.setView?.(b.dataset.view));
    E.viewerShot.addEventListener('click', () => this.screenshot());

    E.pbFirst.addEventListener('click', () => this.userShowIndex(0));
    E.pbPrev.addEventListener('click', () => this.step(-1));
    E.pbPlay.addEventListener('click', () => this.togglePlay());
    E.pbNext.addEventListener('click', () => this.step(1));
    E.pbLast.addEventListener('click', () => this.userShowIndex(this.iters.length - 1));
    E.pbRange.addEventListener('input', () => this.userShowIndex(Number(E.pbRange.value)));
    E.pbSpeed.addEventListener('change', () => { this.speed = Number(E.pbSpeed.value) || 4; });
    E.pbFollow.addEventListener('change', () => this.setFollow(E.pbFollow.checked));

    E.wsRun.addEventListener('click', () => this.h.onRunConfig?.());
    E.wsClose.addEventListener('click', () => this.h.onClosePreview?.());
    E.wsCancel.addEventListener('click', () => this.session && this.h.onCancel?.(this.session.status || { id: this.session.id }));
    E.wsDelete.addEventListener('click', () => this.session && this.h.onDelete?.(this.session.status || { id: this.session.id }));
    E.wsEdit.addEventListener('click', async () => {
      const s = this.session;
      if (!s) return;
      // the submitted input.json (config.json is normalized and drops e.g. paper_reference)
      let cfg = null;
      try { cfg = await api.inputConfig(s.id); } catch { cfg = s.config; }
      if (cfg) this.h.onEditConfig?.(cfg, `Loaded the configuration of run ${s.id}. Edit it and press Run to start a new run.`);
      else toast('The configuration of this run is not available.', 'error');
    });

    E.logDetails.addEventListener('toggle', () => { if (E.logDetails.open) this.fetchLog(); this.updateLogTimer(); });
    E.logRefresh.addEventListener('click', () => this.fetchLog());
    E.logHideEvents.addEventListener('change', () => this.renderLog());
    E.historyDetails.addEventListener('toggle', () => this.renderTable());

    // Keyboard shortcuts. A control reached with Tab keeps its own keys (Space toggles a checkbox or presses a
    // button, arrows move a slider or change a select). After a click on a control (a layer toggle, the speed menu,
    // the playback slider, a run in the list) the shortcuts win, so that the click does not swallow them. Text
    // fields and the New run panel never trigger them.
    this.tabFocused = new WeakSet();
    let lastKeyWasTab = false;
    document.addEventListener('keydown', (ev) => { lastKeyWasTab = ev.key === 'Tab'; }, true);
    document.addEventListener('pointerdown', () => { lastKeyWasTab = false; }, true);
    document.addEventListener('focusin', (ev) => {
      if (lastKeyWasTab) this.tabFocused.add(ev.target); else this.tabFocused.delete(ev.target);
    }, true);
    document.addEventListener('keydown', (ev) => this.onShortcut(ev));
    // after a mouse click on a button, drop its focus (no lingering focus ring, Enter does not repeat the click)
    this.root.addEventListener('click', (ev) => {
      const b = ev.target.closest('button');
      if (b && ev.detail > 0) b.blur();
    });
  }

  onShortcut(ev) {
    if (this.mode !== 'run' || ev.defaultPrevented || ev.altKey || ev.ctrlKey || ev.metaKey) return;
    const key = ev.key === ' ' || ev.code === 'Space' ? ' ' : ev.key;
    if (!SHORTCUT_KEYS.has(key) || document.querySelector('dialog[open]')) return;
    const t = ev.target instanceof Element && ev.target !== document.body ? ev.target : null;
    if (t) {
      if (isTextEntry(t) || t.closest('#newrun-section')) return;
      if (this.tabFocused.has(t) && hasNativeKey(t, key)) return;
      if (t.tagName === 'PRE' && key !== ' ') return; // the log scrolls with the arrows, Home and End
    }
    ev.preventDefault();
    if (key === ' ') { if (!ev.repeat) this.togglePlay(); }
    else if (key === 'ArrowLeft') this.step(-1);
    else if (key === 'ArrowRight') this.step(1);
    else if (key === 'Home') this.userShowIndex(0);
    else this.userShowIndex(this.iters.length - 1);
  }

  onThemeChange() {
    this.viewer.applyTheme?.();
  }

  // ===================================================================================== modes

  setPanels(mode) {
    const run = mode === 'run';
    this.e.playback.hidden = !run;
    this.e.metrics.hidden = !run;
    this.e.charts.hidden = !run;
    this.bottom.hidden = !run;
    this.e.problem.hidden = run;
    for (const cb of $$('.viewer-toolbar input[data-layer="voxels"], .viewer-toolbar input[data-layer="primitives"], .viewer-toolbar input[data-layer="initial"]')) cb.disabled = !run;
    this.e.vtClip.disabled = !run;
    this.e.thrRange.disabled = !run;
    for (const id of ['wsRun', 'wsClose']) this.e[id].hidden = run;
    for (const id of ['wsCancel', 'wsDelete', 'wsEdit']) this.e[id].hidden = true;
  }

  setTitle(name) {
    document.title = name ? `${name} · MSD Shape Optimization` : 'MSD Shape Optimization';
  }

  showEmpty() {
    this.closeSession();
    this.preview = null;
    this.mode = 'empty';
    this.setTitle(null);
    this.viewer.clearProblem?.();
    this.root.hidden = true;
    this.emptyState.hidden = false;
  }

  showPreview({ meta, config, warnings }) {
    this.closeSession();
    this.mode = 'preview';
    this.preview = { meta, config, warnings: warnings || [], stale: false };
    this.root.hidden = false;
    this.emptyState.hidden = true;
    this.setPanels('preview');
    const E = this.e;
    E.wsEyebrow.textContent = 'Preview · not started';
    E.wsTitle.textContent = meta.name || config.name || 'unnamed';
    this.setTitle(`Preview: ${E.wsTitle.textContent}`);
    E.wsChip.replaceChildren(...stateChip({ state: 'preview' }).childNodes);
    E.wsChip.className = 'chip st-preview';
    E.wsLive.hidden = true;
    const opt = meta.optimization || {};
    this.renderSub([methodLong(opt.method), `${fmtGrid(meta.grid?.resolution)} voxels`, `target volume ${opt.volume_fraction}`,
      `up to ${opt.max_iterations} iterations`, `initial radius ${opt.initial_radius_voxels} voxels`]);
    this.renderPreviewAlerts();
    if (this.viewer.available) {
      this.viewer.setProblem(meta);
      this.viewer.setFrame({ theta: null, positions: null, vertices: meta.skeleton?.vertices || [] });
    }
    this.renderProblem(meta, config);
    if (!this.webglMissing) this.setNotice(null);
    this.updateCaption();
  }

  markPreviewStale() {
    const p = this.preview;
    if (this.mode !== 'preview' || !p || (p.stale && !p.invalid)) return;
    p.stale = true;
    p.invalid = null; // the edit may have fixed the problem: back to "edited after this preview"
    this.renderPreviewAlerts();
  }

  /** The edited configuration failed validation while the preview of an earlier one is on screen.
   *  canLocate: the error names an entry of the configuration that the editor can select. */
  markPreviewInvalid(message, canLocate = false) {
    const p = this.preview;
    if (this.mode !== 'preview' || !p) return;
    p.stale = true;
    p.invalid = message;
    p.canLocate = canLocate;
    this.renderPreviewAlerts();
  }

  renderPreviewAlerts() {
    const box = this.e.wsAlerts;
    box.replaceChildren();
    const p = this.preview;
    if (!p) return;
    if (p.invalid) {
      const actions = [];
      if (p.canLocate) {
        const show = el('button', { class: 'btn btn-xs', type: 'button' }, icon('edit'), 'Show in editor');
        show.addEventListener('click', () => this.h.onShowConfigError?.());
        actions.push(show);
      }
      box.append(this.alert('error', 'The edited configuration is invalid',
        el('div', {}, el('span', { class: 'err-text' }, p.invalid),
          el('div', { class: 'alert-note' }, 'The preview below still shows the last configuration that passed validation.')), actions));
    } else if (p.stale) {
      box.append(this.alert('info', 'The configuration was edited after this preview.',
        'Press Validate / Preview again to update it. Run always uses the current editor content.'));
    }
    if (p.warnings.length) {
      box.append(this.alert('warning', `Warning${p.warnings.length > 1 ? 's' : ''} from the validation${p.stale ? ' of this preview' : ''}`,
        el('ul', {}, p.warnings.map((w) => el('li', {}, w)))));
    }
  }

  renderProblem(meta, config) {
    const g = meta.grid || {};
    const sk = meta.skeleton || {};
    const bc = meta.boundary_conditions || {};
    const opt = meta.optimization || {};
    const c = g.cell_size || 0.01;
    const res = g.resolution || [0, 0, 0];
    const tf = bc.total_force || [0, 0, 0];
    const flipped = sk.flipped_edges || [];
    const rows = [
      ['Grid', `${fmtGrid(res)} voxels (${fmtInt(g.num_voxels)}), cell size ${c}`],
      ['Domain', `${res.map((n) => (n * c).toFixed(2)).join(' × ')} (world units)`],
      ['Skeleton', `${(sk.vertices || []).length} vertices, ${(sk.edges || []).length} edges${flipped.length ? ` (edge${flipped.length > 1 ? 's' : ''} ${flipped.join(', ')} reversed for the optimizer)` : ''}`],
      ['Fixed', `${fmtInt((bc.fixed_vertices || []).length)} grid vertices`],
      ['Loads', `${fmtInt((bc.loads || []).length)} grid vertices, total force (${tf.map((v) => fmtNum(v, 4)).join(', ')})`],
      ['Method', methodLong(opt.method)],
      ['Target volume', `${opt.volume_fraction} of the domain`],
      ['Iterations', `at most ${opt.max_iterations} (converged when max change < 10⁻⁵)`],
      ['Initial radius', `${opt.initial_radius_voxels} voxels`],
    ];
    const pr = config && config.paper_reference;
    if (pr) {
      rows.push(['Paper', `${[pr.figure, pr.column ? `column ${pr.column}` : null].filter(Boolean).join(', ')}: E = ${pr.energy_msd ?? '–'} (MSD), ${pr.energy_ma ?? '–'} (MA)`]);
    }
    this.e.problemKv.replaceChildren(...rows.flatMap(([k, v]) => [el('dt', {}, k), el('dd', {}, v)]));
    this.e.problemWarnings.replaceChildren();
  }

  async openRun(id) {
    if (this.mode === 'run' && this.session && this.session.id === id) return;
    this.closeSession();
    this.preview = null;
    this.mode = 'run';
    this.root.hidden = false;
    this.emptyState.hidden = true;
    this.setPanels('run');
    this.viewer.clearProblem?.();
    this.resetRunView(id);

    const sess = new RunSession(id, {
      onLoad: (s) => this.onLoad(s),
      onMeta: (m) => this.onMeta(m),
      onIterations: (batch) => this.onIterations(batch),
      onStatus: () => this.onStatus(),
      onEnd: (s) => this.onEnd(s),
      onConnection: (st) => this.onConnection(st),
      onGone: () => { toast(`Run ${id} no longer exists.`, 'error'); this.h.onMissing?.(id); },
      onError: (e) => toast(`Run ${id}: ${e.message}`, 'error'),
    });
    this.session = sess;
    try {
      await sess.load();
    } catch (e) {
      if (sess !== this.session || (e && e.name === 'AbortError')) return;
      this.setNotice(null);
      if (e.status === 404) {
        toast(`Run ${id} does not exist (it may have been deleted).`, 'error');
        this.h.onMissing?.(id);
        return;
      }
      this.e.wsAlerts.replaceChildren(this.alert('error', 'Could not load this run', e.message));
    }
  }

  resetRunView(id) {
    const E = this.e;
    this.pause();
    this.iters = [];
    this.cur = null;
    this.follow = false;
    this.wantFrame = null;
    this.shownFrame = null;
    this.frameInfo = null;
    this.logLines = [];
    this.provisional = false;
    this.provisionalMeta = null;
    this.watchedLive = false;
    E.wsEyebrow.textContent = 'Run';
    E.wsTitle.textContent = id;
    E.wsChip.className = 'chip st-loading';
    E.wsChip.replaceChildren(...stateChip({ state: 'loading' }).childNodes);
    E.wsLive.hidden = true;
    E.wsSub.replaceChildren();
    E.wsAlerts.replaceChildren();
    E.logPre.textContent = '';
    E.historyWrap.replaceChildren();
    E.fileList.replaceChildren();
    delete E.fileList.dataset.key; // renderDownloads() skips unchanged lists: the emptied list must be rebuilt
    this.charts.setRecords([]);
    this.charts.setCurrent(null);
    this.renderChartEmpty();
    this.renderMetrics(null);
    this.updatePlaybackUI();
    this.setNotice(this.webglMissing ? this.e.viewerNotice.textContent : 'Loading the run…');
    this.updateCaption();
  }

  closeSession() {
    clearInterval(this.logTimer);
    clearTimeout(this.frameRetryTimer);
    this.pause();
    if (this.session) this.session.close();
    this.session = null;
    // a frame loop of the old session ends by itself (it checks the session); let a new one start
    this.loadingFrame = false;
    this.wantFrame = null;
    this.shownFrame = null;
    this.frameRetries = 0;
  }

  // ===================================================================================== run events

  onLoad(s) {
    if (s !== this.session) return;
    if (!this.webglMissing) this.setNotice(null);
    // the recorded iterations first: the header chip and the alerts ("cancelled after iteration k") depend on them
    this.syncIterations();
    this.renderHeader();
    this.renderAlerts();
    if (s.meta) this.viewer.setProblem?.(s.meta);
    else this.loadProvisionalProblem(s);
    const active = !isTerminal(s.state);
    this.watchedLive = active;
    this.setFollow(active, { quiet: true });
    if (this.iters.length) this.showIteration(this.iters[this.iters.length - 1]);
    else { this.renderMetrics(null); this.updateCaption(); }
    this.renderDownloads();
    if (s.state === 'failed' || s.state === 'interrupted') this.e.logDetails.open = true;
    if (this.e.logDetails.open) this.fetchLog();
    this.updateLogTimer();
    this.renderTable();
  }

  async loadProvisionalProblem(s) {
    // a queued run has no meta.json yet: preview the problem from its configuration meanwhile
    if (!this.viewer.available) return;
    const nothing = () => {
      if (s === this.session && !s.meta && isTerminal(s.state)) {
        this.setNotice('The optimizer stopped before it set up the problem, so there is nothing to show in 3D. The log below has the details.');
      }
    };
    if (!s.config) { nothing(); return; }
    try {
      const res = await api.validate(s.config);
      if (s !== this.session || s.meta) return;
      if (!res.ok || !res.meta) { nothing(); return; }
      this.viewer.setProblem(res.meta);
      this.provisional = true;
      this.provisionalMeta = res.meta;
      this.renderHeader();
      this.updateCaption();
    } catch { nothing(); /* for live runs the meta event arrives when the optimizer starts */ }
  }

  onMeta(meta) {
    if (!this.viewer.available) return;
    if (!this.viewer.problem || this.provisional) {
      this.viewer.setProblem(meta, { keepCamera: true });
      this.provisional = false;
      this.shownFrame = null;
      this.frameInfo = null; // the rebuilt scene shows no frame yet
      if (this.cur != null) this.requestFrame(this.cur);
    }
    this.renderAlerts();
  }

  onIterations() {
    this.syncIterations();
    if (this.follow && this.iters.length) this.showIteration(this.iters[this.iters.length - 1]);
    else this.updatePlaybackUI();
    this.renderHeader();
    if (isTerminal(this.session?.state)) this.renderAlerts(); // e.g. "cancelled; iterations 0-k were recorded"
    this.renderDownloads();
    if (this.e.historyDetails.open) this.renderTable();
  }

  onStatus() {
    this.renderHeader();
    this.renderAlerts();
    this.renderChartEmpty(); // "waiting in the queue" -> "setting up the problem"
    this.renderDownloads();
    this.updateLogTimer();
    this.updatePlaybackUI();
    if (this.cur == null) { this.renderMetrics(null); this.updateCaption(); }
    this.h.onRunsChanged?.(); // the run list shows the same state: do not let it lag until its next poll
  }

  onEnd(s) {
    if (s !== this.session) return;
    const following = this.follow;
    this.syncIterations();
    this.setFollow(false, { quiet: true });
    // a run watched live ends on its final state
    if ((following || this.cur == null) && this.iters.length) this.showIteration(this.iters[this.iters.length - 1]);
    else if (this.cur != null) this.renderMetrics(s.history.get(this.cur));
    this.renderHeader();
    this.renderAlerts();
    this.renderDownloads();
    this.updateLogTimer();
    if (this.e.logDetails.open || s.state === 'failed') { this.e.logDetails.open = this.e.logDetails.open || s.state === 'failed'; this.fetchLog(); }
    if (this.e.historyDetails.open) this.renderTable();
    if (this.watchedLive) {
      const st = s.status || {};
      const last = s.history.get(this.iters[this.iters.length - 1]);
      if (st.state === 'completed') toast(`Run ${st.name || s.id} completed${last ? `: compliance ${fmtNum(last.compliance, 4)}` : ''}.`, 'ok');
      else if (st.state === 'failed') toast(`Run ${st.name || s.id} failed: ${st.error || 'see the log'}`, 'error');
      else if (st.state === 'cancelled') toast(`Run ${st.name || s.id} was cancelled.`, 'info');
    }
    this.watchedLive = false;
    this.h.onRunsChanged?.();
  }

  onConnection(state) {
    const live = this.e.wsLive;
    if (state === 'live') { live.hidden = false; live.className = 'live'; live.textContent = 'Live'; live.title = 'Receiving live updates'; }
    else if (state === 'reconnecting') { live.hidden = false; live.className = 'live is-off'; live.textContent = 'Reconnecting…'; live.title = 'The live connection was interrupted; retrying'; }
    else live.hidden = true;
  }

  // ===================================================================================== header, alerts

  renderSub(parts, id) {
    const items = parts.filter(Boolean).map((p) => el('span', {}, p));
    if (id) items.push(el('code', { title: 'run id' }, id));
    this.e.wsSub.replaceChildren(...items);
  }

  lastIteration() { return this.iters.length ? this.iters[this.iters.length - 1] : null; }

  renderHeader() {
    const s = this.session;
    if (!s || !s.status) return;
    const st = s.status;
    const E = this.e;
    E.wsTitle.textContent = st.name || s.id;
    this.setTitle(`${st.name || s.id} (${st.state})`);
    const chip = stateChip({ ...st, iteration: this.lastIteration() ?? st.iteration });
    E.wsChip.className = chip.className;
    E.wsChip.title = chip.title;
    E.wsChip.replaceChildren(...chip.childNodes);
    const times = [`created ${fmtWhen(st.created)}`];
    if (st.finished && isTerminal(st.state)) times.push(`finished ${fmtWhen(st.finished)}`);
    this.renderSub([methodLong(st.method), st.resolution ? `${fmtGrid(st.resolution)} voxels (${fmtInt(st.num_voxels)})` : null,
      isNum(this.targetVf()) ? `target volume ${this.targetVf()}` : null,
      st.threads ? `${st.threads} threads` : 'all cores', ...times], s.id);
    E.wsSub.title = `created ${fmtWhenFull(st.created)}${st.started ? `\nstarted ${fmtWhenFull(st.started)}` : ''}${st.finished ? `\nfinished ${fmtWhenFull(st.finished)}` : ''}`;
    E.wsCancel.hidden = isTerminal(st.state);
    E.wsDelete.hidden = !isTerminal(st.state);
    E.wsEdit.hidden = !s.config;
  }

  alert(kind, title, body, actions = []) {
    const ic = { error: 'alert', warning: 'alert', serious: 'alert', ok: 'check', info: 'info' }[kind] || 'info';
    const msg = el('div', { class: 'msg' }, el('strong', {}, title));
    if (typeof body === 'string') msg.append(el('div', {}, body));
    else if (body) msg.append(body);
    if (actions.length) msg.append(el('div', { class: 'alert-actions' }, actions));
    return el('div', { class: `alert is-${kind}`, role: kind === 'error' ? 'alert' : null }, icon(ic), msg);
  }

  renderAlerts() {
    const s = this.session;
    if (!s || !s.status) return;
    const st = s.status;
    const box = this.e.wsAlerts;
    box.replaceChildren();
    const last = this.lastIteration();
    if (st.state === 'failed') {
      const btn = el('button', { class: 'btn btn-xs', type: 'button' }, 'Show the log');
      btn.addEventListener('click', () => { this.e.logDetails.open = true; this.e.logDetails.scrollIntoView({ behavior: 'smooth', block: 'start' }); });
      box.append(this.alert('error', 'The optimization failed', el('span', { class: 'err-text' }, st.error || 'unknown error'), [btn]));
    } else if (st.state === 'interrupted') {
      box.append(this.alert('serious', 'Interrupted', st.error || 'The server stopped while this run was in progress.'));
    } else if (st.state === 'cancelled') {
      const max = st.max_iterations ?? s.meta?.optimization?.max_iterations;
      const stopped = isNum(max) && last >= max ? 'Stopped after the last iteration' : `Stopped while computing iteration ${last + 1}`;
      box.append(this.alert('info', 'Cancelled', last != null
        ? `${stopped}; iteration${last ? `s 0–${last} were` : ' 0 was'} recorded and can be replayed.`
        : 'The run was cancelled before the optimizer produced an iteration.'));
    } else if (st.state === 'queued') {
      const pos = Number.isInteger(st.queue_position) ? ` (position ${st.queue_position + 1} in the queue)` : '';
      box.append(this.alert('info', 'Waiting for a free optimizer slot', `The run starts automatically${pos}. The problem is previewed from its configuration.`));
    }
    const warnings = (st.warnings && st.warnings.length ? st.warnings : (s.meta && s.meta.warnings)) || [];
    if (warnings.length) {
      box.append(this.alert('warning', `Configuration warning${warnings.length > 1 ? 's' : ''}`, el('ul', {}, warnings.map((w) => el('li', {}, w)))));
    }
  }

  // ===================================================================================== iterations, playback

  syncIterations() {
    const s = this.session;
    if (!s) return;
    const recs = s.records();
    this.iters = recs.map((r) => r.iteration);
    // show the charts before they render: a chart measures its width when it draws (hidden = 0 px = not drawn)
    this.renderChartEmpty();
    this.charts.setRecords(recs);
    this.charts.setCurrent(this.cur);
    this.updatePlaybackUI();
  }

  /** The charts, or why there is nothing to chart yet (depends on the run state, refreshed on status changes). */
  renderChartEmpty() {
    const s = this.session;
    const n = this.iters.length;
    this.e.chartEmpty.hidden = n > 0;
    this.e.chartStack.hidden = n === 0;
    if (n) return;
    const st = s && s.state;
    this.e.chartEmpty.textContent = !st ? 'Loading…' : st === 'queued' ? 'No iterations yet: the run is waiting in the queue.'
      : st === 'running' ? 'No iterations yet: the optimizer is setting up the problem.' : 'No iterations were recorded.';
  }

  isActive() { return !!(this.session && this.session.status && !isTerminal(this.session.state)); }

  setFollow(on, { quiet = false } = {}) {
    this.follow = !!on && this.isActive();
    this.e.pbFollow.checked = this.follow;
    if (this.follow) {
      this.pause();
      if (!quiet && this.iters.length) this.showIteration(this.iters[this.iters.length - 1]);
    }
    this.updatePlaybackUI();
  }

  updatePlaybackUI() {
    const E = this.e;
    const n = this.iters.length;
    const idx = this.cur == null ? -1 : this.iters.indexOf(this.cur);
    E.pbRange.max = String(Math.max(0, n - 1));
    E.pbRange.value = String(Math.max(0, idx));
    E.pbRange.disabled = n < 2;
    E.pbFirst.disabled = E.pbPrev.disabled = idx <= 0;
    E.pbNext.disabled = E.pbLast.disabled = idx < 0 || idx >= n - 1;
    E.pbPlay.disabled = n < 2;
    E.pbFollow.disabled = !this.isActive();
    E.pbFollow.parentElement.hidden = !this.isActive();
    const max = this.session?.status?.max_iterations ?? this.session?.meta?.optimization?.max_iterations;
    if (this.cur == null) E.pbLabel.textContent = n ? '–' : 'no iterations';
    else E.pbLabel.replaceChildren('Iteration ', el('strong', {}, String(this.cur)), max ? ` / ${max}` : '');
    E.pbRange.setAttribute('aria-valuetext', this.cur == null ? 'none' : `iteration ${this.cur}`);
    const use = E.pbPlay.querySelector('use');
    use.setAttribute('href', this.playing ? '#i-pause' : '#i-play');
    E.pbPlay.setAttribute('aria-label', this.playing ? 'Pause' : 'Play');
  }

  showIteration(k) {
    const s = this.session;
    if (!s || !s.history.has(k)) return;
    this.cur = k;
    this.renderMetrics(s.history.get(k));
    this.charts.setCurrent(k);
    this.updatePlaybackUI();
    this.highlightTableRow(k);
    this.requestFrame(k);
    this.updateCaption();
  }

  userShow(k) {
    this.setFollow(false, { quiet: true });
    this.pause();
    this.showIteration(k);
  }

  userShowIndex(i) {
    if (!this.iters.length) return;
    this.userShow(this.iters[clamp(i, 0, this.iters.length - 1)]);
  }

  step(d) {
    if (!this.iters.length) return;
    const i = this.cur == null ? -1 : this.iters.indexOf(this.cur);
    this.userShowIndex((i < 0 ? this.iters.length - 1 : i) + d);
  }

  togglePlay() { if (this.playing) this.pause(); else this.play(); }

  play() {
    if (this.iters.length < 2) return;
    this.setFollow(false, { quiet: true });
    const i = this.cur == null ? -1 : this.iters.indexOf(this.cur);
    if (i < 0 || i >= this.iters.length - 1) this.showIteration(this.iters[0]); // replay from the start
    this.playing = true;
    this.updatePlaybackUI();
    clearTimeout(this.playTimer);
    this.playTimer = setTimeout(() => this.tick(), 1000 / this.speed);
  }

  pause() {
    this.playing = false;
    clearTimeout(this.playTimer);
    if (this.e) this.updatePlaybackUI();
  }

  tick() {
    if (!this.playing) return;
    const i = this.iters.indexOf(this.cur);
    if (i >= this.iters.length - 1) {
      if (this.isActive()) { this.playTimer = setTimeout(() => this.tick(), 500); return; } // wait for new iterations
      this.pause();
      return;
    }
    // advance only once the current frame is on screen, so that a replay shows every iteration
    if (this.loadingFrame || this.shownFrame !== this.cur) { this.playTimer = setTimeout(() => this.tick(), 30); return; }
    this.showIteration(this.iters[i + 1]);
    this.session.frames.prefetch(this.iters.slice(i + 2, i + 6));
    this.playTimer = setTimeout(() => this.tick(), 1000 / this.speed);
  }

  // ===================================================================================== frames

  requestFrame(k) {
    this.wantFrame = k;
    if (!this.viewer.available) return;
    if (!this.loadingFrame) this.pumpFrames();
  }

  async pumpFrames() {
    this.loadingFrame = true;
    const sess = this.session;
    let slow = null;
    try {
      while (sess === this.session && this.wantFrame != null && this.wantFrame !== this.shownFrame) {
        const k = this.wantFrame;
        if (!this.viewer.problem) break; // meta not there yet; onMeta() asks again
        if (!sess.frames.has(k)) slow = setTimeout(() => this.updateCaption(true), 250);
        let entry;
        let failure = null;
        try {
          entry = await sess.frames.get(k);
        } catch (e) {
          if (sess !== this.session || (e && e.name === 'AbortError')) return;
          failure = e;
          entry = { theta: null, positions: null, vertices: null, missing: true };
        } finally {
          clearTimeout(slow);
        }
        if (sess !== this.session) return;
        if (failure) {
          // A network or server error (missing files are not errors: they come back as an empty frame). Keep the
          // frame that is on screen instead of wiping the shape, say so, and try again a little later.
          if (this.wantFrame !== k) continue;
          this.frameFailed(k, failure);
          break;
        }
        // A newer iteration may have been requested while this frame was downloading (live iterations arriving
        // faster than the downloads, fast scrubbing). The finished download is still drawn when it brings the view
        // closer to the wanted iteration; only the requests in between are skipped. (Discarding it would leave a
        // slow connection without any frame for as long as a fast run keeps going.)
        if (this.wantFrame !== k && !this.closerThanShown(k)) continue;
        this.shownFrame = k;
        this.frameRetries = 0;
        this.applyFrame(k, entry);
        this.updateCaption();
      }
    } finally {
      if (sess === this.session) {
        this.loadingFrame = false;
        this.updateCaption();
      }
    }
  }

  /** The frame of the wanted iteration k could not be downloaded: notice + retry with a growing delay. */
  frameFailed(k, error) {
    const sess = this.session;
    const delay = Math.min(30000, 3000 * 2 ** (this.frameRetries || 0));
    this.frameRetries = (this.frameRetries || 0) + 1;
    const showing = this.frameInfo ? ` The 3D view still shows iteration ${this.frameInfo.k}.` : '';
    this.setNotice(`Could not load the frame of iteration ${k}: ${error.message}${showing} Retrying in ${Math.round(delay / 1000)} s…`, 'corner');
    clearTimeout(this.frameRetryTimer);
    this.frameRetryTimer = setTimeout(() => {
      if (sess === this.session && this.wantFrame === k && this.shownFrame !== k && !this.loadingFrame) this.requestFrame(k);
    }, delay);
  }

  /** Whether drawing the frame of iteration k brings the 3D view closer to the wanted iteration. */
  closerThanShown(k) {
    const want = this.wantFrame;
    if (want == null) return false;
    if (this.shownFrame == null || !this.frameInfo) return true;
    return Math.abs(k - want) < Math.abs(this.shownFrame - want);
  }

  applyFrame(k, entry) {
    const rec = this.session.history.get(k);
    this.viewer.setFrame({ theta: entry.theta, positions: entry.positions, vertices: entry.vertices || (rec && rec.vertices) });
    // the viewer drops data that does not fit the problem (e.g. a truncated theta file): say so instead of
    // silently showing an empty domain
    const P = this.viewer.problem;
    const badTheta = !!(entry.theta && P && entry.theta.length !== P.numVoxels);
    const badPositions = !!(entry.positions && P && P.prim && !P.prim.hasData);
    this.frameInfo = { k, missing: entry.missing, noTheta: !entry.theta || badTheta };
    let note = null;
    if (entry.missing) note = `No frame files for iteration ${k}: only the skeleton is shown.`;
    else if (badTheta) {
      note = `The voxel densities of iteration ${k} are damaged (${fmtInt(entry.theta.length)} bytes instead of ${fmtInt(P.numVoxels)}): no voxels are shown.`;
    } else if (!entry.theta) note = `No voxel densities for iteration ${k} (its theta file is missing): no voxels are shown.`;
    else if (this.viewer.layers.primitives && P && P.prim && (badPositions || !entry.positions)) {
      note = badPositions ? `The primitive data of iteration ${k} do not match the problem: no primitives are shown.`
        : `No primitive data for iteration ${k} (its frame file is missing).`;
    }
    if (note) this.setNotice(note, 'corner');
    else if (this.noticeKind === 'corner') this.setNotice(null);
  }

  // ===================================================================================== viewer chrome

  setNotice(text, kind = 'center', timeout = 0) {
    const n = this.e.viewerNotice;
    clearTimeout(this.noticeTimer);
    if (!text) {
      if (this.webglMissing) return;
      n.hidden = true;
      this.noticeKind = null;
      return;
    }
    n.textContent = text;
    n.className = `viewer-notice${kind === 'corner' ? ' is-corner' : ''}`;
    n.hidden = false;
    this.noticeKind = kind;
    if (timeout) this.noticeTimer = setTimeout(() => this.setNotice(null), timeout);
  }

  updateCaption(loading = false) {
    const cap = this.e.viewerCaption;
    const strong = (t) => el('strong', {}, t);
    if (this.mode === 'preview' && this.preview) {
      const bc = this.preview.meta.boundary_conditions || {};
      cap.replaceChildren(strong('Problem preview'), ` · ${fmtInt((bc.fixed_vertices || []).length)} fixed, ${fmtInt((bc.loads || []).length)} loaded grid vertices`);
      return;
    }
    if (this.mode !== 'run' || !this.session) { cap.replaceChildren(); return; }
    const st = this.session.state;
    if (this.cur == null) {
      const msg = !st ? 'Loading…' : st === 'queued' ? 'Waiting in the queue' : st === 'running' ? 'Waiting for the first iteration' : 'No iterations recorded';
      cap.replaceChildren(strong(msg), this.provisional ? ' · problem preview' : '');
      return;
    }
    const max = this.session.status?.max_iterations ?? this.session.meta?.optimization?.max_iterations;
    const rec = this.session.history.get(this.cur);
    const parts = [strong(`Iteration ${this.cur}`), max ? ` / ${max}` : ''];
    // the compliance belongs next to the shape (on narrower screens the metrics card is below the view)
    if (rec && isNum(rec.compliance)) parts.push(' · E\u00a0=\u00a0', strong(fmtNum(rec.compliance, 4)));
    const info = this.viewer.info;
    const shownK = this.frameInfo ? this.frameInfo.k : null;
    const shown = shownK === this.cur;
    if (!shown) {
      // the previous frame stays on screen while the frame of the displayed iteration downloads (or after its
      // download failed): say which iteration the 3D view shows
      if (loading || this.loadingFrame) parts.push(shownK != null ? ` · 3D view at iteration ${shownK}, loading ${this.cur}…` : ' · loading frame…');
      else if (shownK != null) parts.push(` · 3D view at iteration ${shownK}`);
    } else if (this.viewer.layers?.voxels && info && info.filled != null) {
      const t = Number(this.e.thrRange.value);
      if (Math.abs(t - 0.5) < 1e-6 && rec && isNum(rec.filled_voxels)) {
        // At the default threshold the optimizer's own count of theta > 0.5 (= "Filled voxels" in the metrics). The
        // drawn set comes from the 8-bit densities and can miss the few voxels with theta within 0.002 above 0.5.
        parts.push(` · ${fmtInt(rec.filled_voxels)} filled voxels (θ\u00a0>\u00a00.5)`);
      } else {
        parts.push(` · ${fmtInt(info.filled)} voxels with θ\u00a0>\u00a0${t.toFixed(2)}`);
      }
    }
    cap.replaceChildren(...parts);
  }

  updateLegend() {
    const on = (k) => $(`.viewer-toolbar input[data-layer="${k}"]`)?.checked;
    const items = [];
    if (on('skeleton')) items.push(['lg-vertex', 'Skeleton vertex'], ['lg-edge', 'Skeleton edge']);
    if (on('initial')) items.push(['lg-initial', 'Initial skeleton']);
    if (on('primitives')) items.push(['lg-prim', 'Primitives']);
    if (on('bcs')) items.push(['lg-fixed', 'Fixed vertex'], ['lg-load', 'Load']);
    const lg = this.e.viewerLegend;
    lg.replaceChildren(...items.map(([cls, text]) => el('li', {}, el('span', { class: `lg ${cls}` }), text)));
    lg.hidden = items.length === 0;
  }

  screenshot() {
    if (!this.viewer.available || !this.viewer.problem) { toast('Nothing to capture yet.', 'info'); return; }
    const base = (this.session?.status?.name || this.preview?.meta?.name || 'msd').replace(/[^A-Za-z0-9_.-]+/g, '_');
    const name = this.mode === 'run' && this.cur != null ? `${base}_iter${String(this.cur).padStart(4, '0')}.png` : `${base}_preview.png`;
    this.viewer.screenshot(name, (blob) => {
      if (blob) downloadBlob(blob, name);
      else toast('Could not capture the 3D view.', 'error');
    });
  }

  // ===================================================================================== metrics

  /** Target volume fraction. (The run summaries of the API carry the *current* volume fraction under the same
   *  key, so the target comes from meta.json, the provisional meta of a queued run, or the configuration.) */
  targetVf() {
    const s = this.session;
    const v = s?.meta?.optimization?.volume_fraction ?? this.provisionalMeta?.optimization?.volume_fraction
      ?? s?.config?.optimization?.volume_fraction;
    return isNum(v) ? v : null;
  }

  renderMetrics(rec) {
    const E = this.e;
    const s = this.session;
    const st = (s && s.status) || {};
    const meta = s && s.meta;
    const maxIt = st.max_iterations ?? meta?.optimization?.max_iterations;
    const target = this.targetVf();
    const last = this.lastIteration();
    E.mVolumeTarget.textContent = isNum(target) ? target.toFixed(3) : '–';
    if (!rec) {
      E.mCompliance.textContent = '–';
      E.mCompliance.classList.add('is-empty');
      E.mComplianceSub.textContent = !s || !st.state ? 'Loading…' : st.state === 'queued' ? 'The run is waiting in the queue.'
        : st.state === 'running' ? 'Waiting for the first iteration…' : 'No iterations were recorded.';
      for (const k of ['mVolume', 'mEnergy', 'mPenalty', 'mIter', 'mElapsed', 'mChange', 'mFilled', 'mRadius', 'mStep', 'mLs']) E[k].textContent = '–';
      E.mConv.replaceChildren();
      E.mVolumeFill.style.width = '0';
      E.mVolumeTick.hidden = true;
      this.renderPaper(null);
      return;
    }
    E.mCompliance.classList.remove('is-empty');
    E.mCompliance.textContent = fmtNum(rec.compliance, 4);
    const tag = rec.iteration !== last ? '' : st.state === 'completed' ? ' · final'
      : isTerminal(st.state) ? ' · last recorded' : ' · latest';
    E.mComplianceSub.textContent = `at iteration ${rec.iteration}${maxIt ? ` of ${maxIt}` : ''}${tag}${rec.iteration === 0 ? ' (initial state)' : ''}`;

    // volume fraction meter: zero-based, the target as a tick
    const vf = rec.volume_fraction;
    E.mVolume.textContent = isNum(vf) ? vf.toFixed(3) : '–';
    const top = Math.min(1, Math.max(0.1, (target || 0) * 2, (vf || 0) * 1.15));
    E.mVolumeFill.style.width = `${clamp((vf || 0) / top, 0, 1) * 100}%`;
    E.mVolumeTick.hidden = !isNum(target);
    if (isNum(target)) E.mVolumeTick.style.left = `${clamp(target / top, 0, 1) * 100}%`;
    E.mVolumeMeter.setAttribute('aria-label', `Volume fraction ${isNum(vf) ? vf.toFixed(3) : 'unknown'}, target ${isNum(target) ? target : 'unknown'}`);

    E.mEnergy.textContent = fmtNum(rec.energy, 4);
    E.mPenalty.textContent = fmtNum(rec.volume_penalty, 3);
    E.mIter.textContent = `${rec.iteration}${maxIt ? ` / ${maxIt}` : ''}`;
    E.mElapsed.textContent = fmtDuration(rec.elapsed);
    E.mFilled.textContent = fmtInt(rec.filled_voxels);
    E.mRadius.textContent = isNum(rec.radius_min_voxels) ? `${fmtNum(rec.radius_min_voxels, 3)} – ${fmtNum(rec.radius_max_voxels, 3)}` : '–';
    E.mStep.textContent = rec.iteration === 0 ? '–' : sciShort(rec.step_size);
    E.mLs.textContent = rec.iteration === 0 ? '–' : String(rec.line_search_iterations ?? '–');

    E.mChange.textContent = rec.iteration === 0 ? '–' : fmtSci(rec.max_change, 2);
    let conv = null;
    if (rec.iteration > 0 && isNum(rec.max_change) && rec.max_change < CONVERGENCE) {
      conv = el('span', { class: 'conv is-yes' }, icon('check'), 'converged');
    } else if (isTerminal(st.state) && rec.iteration === last && rec.iteration > 0) {
      const why = maxIt && rec.iteration >= maxIt ? ' (iteration budget reached)'
        : st.state !== 'completed' ? ` (run ${st.state})` : '';
      conv = el('span', { class: 'conv' }, s.result && s.result.converged ? [icon('check'), 'converged'] : `not converged${why}`);
      if (s.result && s.result.converged) conv.classList.add('is-yes');
    } else if (rec.iteration === 0) {
      conv = el('span', { class: 'conv' }, 'initial state');
    }
    E.mConv.replaceChildren(...(conv ? [conv] : []));
    this.renderPaper(rec);
  }

  /** The paper's value for this method (configurations with "paper_reference") and the relative difference. */
  renderPaper(rec) {
    const box = this.e.mPaper;
    const s = this.session;
    const pr = (s && s.status && s.status.paper_reference) || (s && s.config && s.config.paper_reference);
    if (!pr || !(isNum(pr.energy_msd) || isNum(pr.energy_ma))) { box.hidden = true; return; }
    const method = s.status?.method || s.meta?.optimization?.method || 'msd';
    const pv = pr[`energy_${method}`];
    const where = [pr.figure, pr.column ? `column ${pr.column}` : null].filter(Boolean).join(', ');
    const val = (m) => el('span', { class: m === method ? 'is-current' : '', style: 'white-space: nowrap' },
      `${methodLabel(m)} ${isNum(pr[`energy_${m}`]) ? pr[`energy_${m}`] : '–'}`);
    const kids = [el('div', {}, `Paper (${where}): E = `, val('msd'), ' · ', val('ma'))];
    if (rec && isNum(pv) && isNum(rec.compliance)) {
      const d = ((rec.compliance - pv) / pv) * 100;
      kids.push(el('div', { class: 'paper-delta' }, 'This iteration vs the paper’s ', methodLabel(method), ' value: ',
        el('strong', { style: 'white-space: nowrap' }, `${fmtSigned(d, 1)}\u00a0%`)));
    }
    box.replaceChildren(...kids);
    box.title = 'The paper reports the final compliance of complete runs (up to 200 iterations).';
    box.hidden = false;
  }

  // ===================================================================================== downloads, log, table

  availableFiles() {
    const s = this.session;
    const set = new Set();
    if (!s || !s.status) return set;
    for (const f of (s.result && s.result.files) || []) set.add(f);
    if (s.meta) set.add('config.json');
    if (s.history.size) { set.add('history.jsonl'); set.add('energy.txt'); }
    if (s.state !== 'queued' && (s.status.started || s.state === 'running' || s.state === 'failed')) set.add('stdout.log');
    return set;
  }

  renderDownloads() {
    const s = this.session;
    if (!s || !s.status) return;
    const avail = this.availableFiles();
    const files = FILES.filter(([n]) => avail.has(n));
    // re-render only when the run, its state or its set of files changed (this runs on every live iteration)
    const list = this.e.fileList;
    const key = JSON.stringify([s.id, s.state, files.map(([n]) => n)]);
    if (list.dataset.key === key) return;
    list.dataset.key = key;
    if (files.length) {
      list.replaceChildren(...files.map(([n, d]) => el('li', {},
        el('a', { href: api.fileUrl(s.id, n), download: `${s.id}_${n}` }, icon('download'),
          el('span', {}, el('span', { class: 'fname' }, n), el('span', { class: 'fdesc' }, d))))));
    } else {
      list.replaceChildren(el('li', { class: 'file-empty' }, s.state === 'queued' ? 'Files appear here once the optimizer has started.'
        : s.state === 'running' ? 'Files appear here once the optimizer has set up the problem.' : 'No files are available for this run.'));
    }
  }

  async fetchLog() {
    const s = this.session;
    if (!s || !this.e.logDetails.open) return;
    try {
      const { lines } = await api.log(s.id, 200, s.ctrl.signal);
      if (s !== this.session) return;
      const first = !this.logLines.length;
      this.logLines = lines || [];
      this.renderLog(first);
    } catch (e) {
      if (s !== this.session || (e && e.name === 'AbortError')) return;
      this.e.logPre.textContent = `Could not load the log: ${e.message}`;
    }
  }

  renderLog(scrollToEnd = false) {
    const pre = this.e.logPre;
    const atEnd = pre.scrollTop + pre.clientHeight >= pre.scrollHeight - 30;
    const hide = this.e.logHideEvents.checked;
    const lines = hide ? this.logLines.filter((l) => !l.startsWith('@@MSD ')) : this.logLines;
    const st = this.session && this.session.state;
    pre.textContent = lines.length ? lines.join('\n') : (st === 'queued' ? 'The run has not started yet.' : 'The log is empty.');
    if (scrollToEnd || atEnd) pre.scrollTop = pre.scrollHeight;
  }

  updateLogTimer() {
    clearInterval(this.logTimer);
    if (this.e.logDetails.open && this.isActive()) this.logTimer = setInterval(() => this.fetchLog(), 3000);
  }

  renderTable() {
    const wrap = this.e.historyWrap;
    if (!this.e.historyDetails.open || !this.session) return;
    const recs = this.session.records();
    if (!recs.length) { wrap.replaceChildren(el('p', { class: 'file-empty', style: 'padding: 10px 12px; margin: 0' }, 'No iterations yet.')); return; }
    const f4 = (v) => fmtNum(v, 4);
    const cols = [
      ['iteration', 'Iteration', (v) => String(v)],
      ['compliance', 'Compliance', f4],
      ['energy', 'Total energy', f4],
      ['volume_penalty', 'Volume penalty', (v) => fmtNum(v, 3)],
      ['volume_fraction', 'Volume fraction', (v) => (isNum(v) ? v.toFixed(4) : '–')],
      ['filled_voxels', 'Filled voxels', fmtInt],
      ['max_change', 'Max change', sciShort],
      ['step_size', 'Step size', sciShort],
      ['line_search_iterations', 'Line search', (v) => (v == null ? '–' : String(v))],
      ['elapsed', 'Elapsed (s)', (v) => (isNum(v) ? v.toFixed(1) : '–')],
    ];
    const thead = el('thead', {}, el('tr', {}, cols.map(([, h]) => el('th', { scope: 'col' }, h))));
    const tbody = el('tbody');
    for (const r of recs) {
      const tr = el('tr', { tabindex: '0', 'data-k': r.iteration }, cols.map(([k, , f]) => el('td', {}, f(r[k]))));
      tbody.append(tr);
    }
    tbody.addEventListener('click', (ev) => {
      const tr = ev.target.closest('tr');
      if (tr) this.userShow(Number(tr.dataset.k));
    });
    tbody.addEventListener('keydown', (ev) => {
      // Enter (and Space on a row reached with Tab) shows the row; after a click, Space stays play / pause
      const tr = ev.target.closest('tr');
      if (tr && (ev.key === 'Enter' || (ev.key === ' ' && this.tabFocused.has(tr)))) {
        ev.preventDefault();
        ev.stopPropagation();
        this.userShow(Number(tr.dataset.k));
      }
    });
    const table = el('table', { class: 'htable' }, el('caption', { class: 'sprite' }, 'Iteration history'), thead, tbody);
    wrap.replaceChildren(table);
    this.highlightTableRow(this.cur);
  }

  highlightTableRow(k) {
    if (!this.e.historyDetails.open) return;
    for (const tr of this.e.historyWrap.querySelectorAll('tr.is-current')) tr.classList.remove('is-current');
    const tr = this.e.historyWrap.querySelector(`tr[data-k="${k}"]`);
    if (tr) tr.classList.add('is-current');
  }
}
