// Convergence charts drawn as plain SVG (no chart library).
//
// A ChartGroup holds the history records and the two shared x positions: the iteration currently displayed in
// the 3D view (a solid marker) and the iteration under the pointer (a hairline crosshair, synchronized across
// all charts). Each LineChart draws one or more series of the records on one y axis (never two y scales).
// Marks follow the data-viz spec: 2px lines, >= 8px dots with a 2px surface ring, hairline solid gridlines,
// a dashed line only for reference values (target / threshold).

import { el, svgEl, isNum, fmtNum, fmtPow10, clamp } from './util.js';

/** The 1-2-5 step closest (on a log scale) to span / count, as in d3: rounding up instead turned e.g. a raw step
 *  of 0.051 into 0.1 and left only two labels on a short axis. */
function niceStep(span, count) {
  const raw = span / Math.max(1, count);
  const p = 10 ** Math.floor(Math.log10(raw));
  const f = raw / p;
  return (f >= Math.sqrt(50) ? 10 : f >= Math.sqrt(10) ? 5 : f >= Math.SQRT2 ? 2 : 1) * p;
}

/** The next smaller 1-2-5 step (5 -> 2, 2 -> 1, 1 -> 0.5). */
function smallerStep(step) {
  const p = 10 ** Math.floor(Math.log10(step) + 1e-9);
  const m = Math.round(step / p);
  return (m >= 5 ? 2 : m >= 2 ? 1 : 0.5) * p;
}

function stepTicks(lo, hi, step) {
  const ticks = [];
  for (let i = Math.ceil(lo / step - 1e-9); i * step <= hi + step * 1e-9; i++) {
    const v = i * step; // multiples, not a running sum: no drift like 0.30000000000000004
    ticks.push(Math.abs(v) < step * 1e-9 ? 0 : v);
  }
  return ticks;
}

/** Nice linear ticks inside [lo, hi]: about `count` of them and at least three when the axis allows it. */
function linearTicks(lo, hi, count) {
  let step = niceStep(hi - lo, count);
  let ticks = stepTicks(lo, hi, step);
  if (ticks.length < 3) {
    step = smallerStep(step);
    ticks = stepTicks(lo, hi, step);
  }
  return { ticks, step };
}

/** Decimals that print every multiple of `step` exactly (2.5 -> 1, so that the tick at 12.5 is not labelled 13). */
function decimalsFor(step) {
  let d = Math.max(0, -Math.floor(Math.log10(step) + 1e-9));
  while (d < 8 && Math.abs(step * 10 ** d - Math.round(step * 10 ** d)) > 1e-6) d++;
  return d;
}

function logTicks(lo, hi, count) {
  const e0 = Math.floor(Math.log10(lo));
  const e1 = Math.ceil(Math.log10(hi));
  const pick = (mults) => {
    const t = [];
    for (let e = e0; e <= e1; e++) {
      for (const m of mults) {
        const v = m * 10 ** e;
        if (v >= lo * (1 - 1e-9) && v <= hi * (1 + 1e-9)) t.push(v);
      }
    }
    return t;
  };
  let t = pick([1]);
  if (t.length > 6) t = t.filter((_, i) => i % Math.ceil(t.length / 6) === 0);
  if (t.length < 3) t = pick([1, 2, 5]);
  // an axis of less than about a decade (e.g. energies between 8 and 18): evenly stepped values read better than
  // the sparse 1-2-5 sequence, which may leave a single label
  if (t.length < 3) t = linearTicks(lo, hi, count + 1).ticks.filter((v) => v > 0);
  return t;
}

/** Labels of log-axis ticks: powers of ten as 10^k when the axis reaches small or large magnitudes. */
function fmtLogTicks(ticks) {
  const isPow = (v) => Math.abs(Math.log10(v) - Math.round(Math.log10(v))) < 1e-9;
  const extreme = Math.min(...ticks) < 0.01 || Math.max(...ticks) >= 1e4;
  if (extreme && ticks.every(isPow)) return ticks.map((v) => fmtPow10(Math.round(Math.log10(v))));
  return ticks.map((v) => (v >= 1e4 || v < 1e-3 ? v.toExponential(0) : String(Number(v.toPrecision(3)))));
}

/** Shared state of the charts of one run. */
export class ChartGroup {
  constructor({ onPick } = {}) {
    this.charts = [];
    this.records = [];
    this.iters = [];
    this.byIter = new Map();
    this.current = null;
    this.hover = null;
    this.onPick = onPick;
  }

  add(chart) {
    chart.group = this;
    this.charts.push(chart);
    return chart;
  }

  setRecords(records) {
    this.records = records;
    this.iters = records.map((r) => r.iteration);
    this.byIter = new Map(records.map((r) => [r.iteration, r]));
    this.xmax = Math.max(1, this.iters.length ? this.iters[this.iters.length - 1] : 1);
    for (const c of this.charts) c.render();
  }

  /** The available iteration closest to k. */
  nearest(k) {
    const a = this.iters;
    if (!a.length) return null;
    let lo = 0;
    let hi = a.length - 1;
    while (hi - lo > 1) {
      const mid = (lo + hi) >> 1;
      if (a[mid] < k) lo = mid; else hi = mid;
    }
    return Math.abs(a[lo] - k) <= Math.abs(a[hi] - k) ? a[lo] : a[hi];
  }

  setCurrent(k) {
    this.current = k;
    for (const c of this.charts) c.renderOverlay();
  }

  setHover(k, source = null) {
    if (k === this.hover && source === this.hoverSource) return;
    this.hover = k;
    this.hoverSource = source;
    for (const c of this.charts) c.renderOverlay();
  }

  setLog(on) {
    for (const c of this.charts) if (c.o.logToggle) { c.log = on; c.render(); }
  }
}

export class LineChart {
  /**
   * opts: title, note, height, yScale ('linear' | 'log'), logToggle (follows the group's log switch),
   * series: [{key, label, color (CSS color), format}], refs(records) -> [{value, label}],
   * include(records) -> extra values the y domain must contain, extraRows(record) -> [{label, value}]
   */
  constructor(figure, opts) {
    this.fig = figure;
    this.o = { height: 150, yScale: 'linear', ...opts };
    this.log = this.o.yScale === 'log';
    this.group = null;

    const cap = el('figcaption', { class: 'chart-cap' }, el('span', { class: 'chart-title' }, this.o.title));
    if (this.o.series.length > 1) {
      // legend for >= 2 series (line keys, text in ink)
      cap.append(el('ul', { class: 'chart-legend' }, this.o.series.map((s) => el('li', {},
        el('span', { class: 'key', style: `--key: ${s.color}` }), s.label))));
    } else if (this.o.note) {
      cap.append(el('span', { class: 'chart-note' }, this.o.note));
    }
    this.plot = el('div', { class: 'chart-plot', tabindex: '0', role: 'img', 'aria-label': this.o.ariaLabel || this.o.title });
    this.svg = svgEl('svg', { 'aria-hidden': 'true' });
    this.tip = el('div', { class: 'tooltip', hidden: true });
    this.plot.append(this.svg, this.tip);
    figure.replaceChildren(cap, this.plot);

    this.gGrid = svgEl('g');
    this.gAxes = svgEl('g');
    this.gRefs = svgEl('g');
    this.gLines = svgEl('g');
    this.gRefLabels = svgEl('g'); // above the series: the labels (with a surface halo) stay readable on a line
    this.gOver = svgEl('g');
    this.hit = svgEl('rect', { fill: 'transparent' });
    this.svg.append(this.gGrid, this.gAxes, this.gRefs, this.gLines, this.gRefLabels, this.gOver, this.hit);

    this.ro = new ResizeObserver(() => this.render());
    this.ro.observe(this.plot);

    const iterAt = (ev) => {
      if (!this.sx || !this.group) return null;
      const rect = this.svg.getBoundingClientRect();
      return this.group.nearest(this.sx.invert(ev.clientX - rect.left));
    };
    let down = null;
    this.hit.addEventListener('pointermove', (ev) => this.group?.setHover(iterAt(ev), this));
    this.hit.addEventListener('pointerleave', () => this.group?.setHover(null, null));
    this.hit.addEventListener('pointerdown', (ev) => { down = { x: ev.clientX, y: ev.clientY }; this.group?.setHover(iterAt(ev), this); });
    this.hit.addEventListener('pointerup', (ev) => {
      // a click (not a drag / scroll) jumps to that iteration
      if (down && Math.hypot(ev.clientX - down.x, ev.clientY - down.y) < 8) {
        const k = iterAt(ev);
        if (k != null) this.group?.onPick?.(k);
      }
      down = null;
      if (ev.pointerType !== 'mouse') this.group?.setHover(null, null);
    });
    this.plot.addEventListener('focus', () => this.renderOverlay());
    this.plot.addEventListener('blur', () => this.renderOverlay());
  }

  values(rec, s) {
    const v = rec[s.key];
    return isNum(v) && (!this.log || v > 0) ? v : null;
  }

  render() {
    const g = this.group;
    const width = Math.round(this.plot.clientWidth);
    if (!g || width < 40) return;
    const H = this.o.height;
    const recs = g.records;
    const refs = this.o.refs ? this.o.refs(recs) : [];

    // ---- y domain
    const vals = [];
    for (const r of recs) for (const s of this.o.series) { const v = this.values(r, s); if (v != null) vals.push(v); }
    for (const ref of refs) if (isNum(ref.value) && (!this.log || ref.value > 0)) vals.push(ref.value);
    if (this.o.include) for (const v of this.o.include(recs)) if (isNum(v) && (!this.log || v > 0)) vals.push(v);
    let lo = vals.length ? Math.min(...vals) : (this.log ? 0.1 : 0);
    let hi = vals.length ? Math.max(...vals) : 1;
    let yt;
    let fmtY;
    if (this.log) {
      if (!(hi > lo)) { lo /= 2; hi *= 2; }
      lo /= 1.12;
      hi *= 1.12;
      yt = logTicks(lo, hi, H > 140 ? 4 : 3);
    } else {
      if (!(hi > lo)) { const p = Math.abs(hi) * 0.1 || 1; lo -= p; hi += p; }
      const pad = (hi - lo) * 0.06;
      const t = linearTicks(lo - pad, hi + pad, H > 140 ? 4 : 3);
      lo = Math.min(lo - pad, t.ticks[0]);
      hi = Math.max(hi + pad, t.ticks[t.ticks.length - 1]);
      yt = t.ticks;
      const d = decimalsFor(t.step);
      fmtY = (v) => v.toFixed(d);
    }
    const yLabels = this.log ? fmtLogTicks(yt) : yt.map(fmtY);

    // ---- geometry
    const ml = Math.max(26, 8 + 6.1 * Math.max(...yLabels.map((s) => s.length), 1));
    const mr = 44;
    const mt = 8;
    const mb = 20;
    const pw = width - ml - mr;
    const ph = H - mt - mb;
    const xmax = g.xmax;
    const sx = (k) => ml + (k / xmax) * pw;
    sx.invert = (px) => clamp(((px - ml) / pw) * xmax, 0, xmax);
    const ly0 = this.log ? Math.log10(lo) : lo;
    const ly1 = this.log ? Math.log10(hi) : hi;
    const sy = (v) => mt + ph - (((this.log ? Math.log10(v) : v) - ly0) / (ly1 - ly0)) * ph;
    Object.assign(this, { sx, sy, ml, mr, mt, mb, pw, ph, width });

    this.svg.setAttribute('width', width);
    this.svg.setAttribute('height', H);
    this.svg.setAttribute('viewBox', `0 0 ${width} ${H}`);
    this.hit.setAttribute('x', ml - 6);
    this.hit.setAttribute('y', 0);
    this.hit.setAttribute('width', Math.max(0, pw + 12));
    this.hit.setAttribute('height', H);

    // ---- grid + axes
    this.gGrid.replaceChildren();
    this.gAxes.replaceChildren();
    let lastY = null;
    yt.forEach((v, i) => {
      const y = Math.round(sy(v)) + 0.5;
      if (y < mt - 1 || y > mt + ph + 1) return;
      if (lastY != null && Math.abs(y - lastY) < 13) return; // labels must not touch (log axes squeeze the top)
      lastY = y;
      this.gGrid.append(svgEl('line', { class: 'grid-line', x1: ml, x2: ml + pw, y1: y, y2: y }));
      const t = svgEl('text', { class: 'tick', x: ml - 6, y, 'text-anchor': 'end', 'dominant-baseline': 'middle' });
      t.textContent = yLabels[i];
      this.gAxes.append(t);
    });
    const xb = mt + ph + 0.5;
    this.gAxes.append(svgEl('line', { class: 'axis-line', x1: ml, x2: ml + pw, y1: xb, y2: xb }));
    // iteration ticks are integers, at least one iteration apart (niceStep gives 1-2-5 steps, never 2.5, whose ticks
    // 2.5, 7.5 would be rounded to the uneven 3, 8)
    const xs = Math.max(1, niceStep(xmax, Math.max(2, Math.floor(pw / 56))));
    const xt = [];
    for (let i = 0; i * xs <= xmax + 1e-9; i++) xt.push(Math.round(i * xs));
    for (const v of xt) {
      const t = svgEl('text', { class: 'tick', x: sx(v), y: H - 5, 'text-anchor': 'middle' });
      t.textContent = String(v);
      this.gAxes.append(t);
    }

    // ---- reference lines (dashed: they are thresholds, not data)
    this.gRefs.replaceChildren();
    this.gRefLabels.replaceChildren();
    for (const ref of refs) {
      if (!isNum(ref.value) || (this.log && ref.value <= 0)) continue;
      const y = sy(ref.value);
      this.gRefs.append(svgEl('line', { class: 'ref-line', x1: ml, x2: ml + pw, y1: y, y2: y }));
      const t = svgEl('text', { class: 'ref-label', x: ml + 4, y: y - 4 });
      t.textContent = ref.label;
      this.gRefLabels.append(t);
    }

    // ---- series
    this.gLines.replaceChildren();
    const ends = [];
    for (const s of this.o.series) {
      let d = '';
      let pen = false;
      let last = null;
      for (const r of recs) {
        const v = this.values(r, s);
        if (v == null) { pen = false; continue; }
        d += `${pen ? 'L' : 'M'}${sx(r.iteration).toFixed(1)},${sy(v).toFixed(1)}`;
        pen = true;
        last = { r, v };
      }
      if (d) this.gLines.append(svgEl('path', { class: 'series', d, style: `stroke: ${s.color}` }));
      if (last) ends.push({ s, ...last, y: sy(last.v) });
    }
    // direct end labels (latest values), skipped where they would collide; the legend carries identity
    const placed = [];
    for (const e of ends) {
      if (placed.some((p) => Math.abs(p - e.y) < 11)) continue;
      placed.push(e.y);
      const t = svgEl('text', { class: 'end-label', x: sx(e.r.iteration) + 6, y: e.y, 'dominant-baseline': 'middle' });
      t.textContent = (e.s.format || fmtNum)(e.v);
      this.gLines.append(t);
    }
    this.renderOverlay();
  }

  /** Current-iteration marker, hover crosshair, dots and tooltip. */
  renderOverlay() {
    const g = this.group;
    if (!g || !this.sx) return;
    this.gOver.replaceChildren();
    const focused = document.activeElement === this.plot;
    const marks = [];
    if (g.current != null && g.byIter.has(g.current)) marks.push({ k: g.current, cls: 'cursor-line' });
    if (g.hover != null && g.hover !== g.current && g.byIter.has(g.hover)) marks.push({ k: g.hover, cls: 'hover-line' });
    for (const m of marks) {
      const x = Math.round(this.sx(m.k)) + 0.5;
      this.gOver.append(svgEl('line', { class: m.cls, x1: x, x2: x, y1: this.mt, y2: this.mt + this.ph }));
      const r = g.byIter.get(m.k);
      for (const s of this.o.series) {
        const v = this.values(r, s);
        if (v == null) continue;
        this.gOver.append(svgEl('circle', { class: 'dot', cx: this.sx(m.k), cy: this.sy(v), r: 4, style: `fill: ${s.color}` }));
      }
    }
    // tooltip: on the chart under the pointer, or on the focused chart for the displayed iteration
    const tipK = g.hoverSource === this && g.hover != null ? g.hover : (focused ? g.current : null);
    if (tipK == null || !g.byIter.has(tipK)) { this.tip.hidden = true; return; }
    this.fillTooltip(g.byIter.get(tipK), g.hoverSource === this);
    this.tip.hidden = false;
    const x = this.sx(tipK);
    const w = this.tip.offsetWidth;
    let left = x + 12;
    if (left + w > this.width - 2) left = x - 12 - w;
    this.tip.style.left = `${Math.max(0, left)}px`;
    this.tip.style.top = `${this.mt}px`;
  }

  fillTooltip(r, hovering) {
    const rows = [el('div', { class: 'tt-head' }, `Iteration ${r.iteration}`)];
    for (const s of this.o.series) {
      const v = r[s.key];
      rows.push(el('div', { class: 'tt-row' }, el('span', { class: 'tt-key', style: `--key: ${s.color}` }),
        el('strong', {}, isNum(v) ? (s.format || fmtNum)(v) : '–'), el('span', {}, s.label)));
    }
    for (const x of this.o.extraRows ? this.o.extraRows(r) : []) {
      rows.push(el('div', { class: 'tt-row' }, el('span', { class: `tt-key${x.ref ? ' is-ref' : ''}`, style: x.ref ? '' : 'border-top-color: transparent' }),
        el('strong', {}, x.value), el('span', {}, x.label)));
    }
    if (hovering) rows.push(el('div', { class: 'tt-hint' }, 'Click to show this iteration'));
    this.tip.replaceChildren(...rows);
  }
}
