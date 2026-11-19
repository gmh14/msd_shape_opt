// Small DOM, formatting and JSON helpers shared by the modules.

export const $ = (sel, root = document) => root.querySelector(sel);
export const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

/** Creates an element. Text children are inserted as text nodes (never as HTML). */
export function el(tag, attrs = {}, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v == null || v === false) continue;
    if (k === 'class') node.className = v;
    else if (k === 'text') node.textContent = v;
    else if (k === 'style' && typeof v === 'object') Object.assign(node.style, v);
    else if (k.startsWith('on') && typeof v === 'function') node.addEventListener(k.slice(2), v);
    else if (v === true) node.setAttribute(k, '');
    else node.setAttribute(k, String(v));
  }
  append(node, children);
  return node;
}

function append(node, children) {
  for (const c of children.flat(Infinity)) {
    if (c == null || c === false) continue;
    node.append(c instanceof Node ? c : document.createTextNode(String(c)));
  }
}

const SVG_NS = 'http://www.w3.org/2000/svg';
export function svgEl(tag, attrs = {}) {
  const node = document.createElementNS(SVG_NS, tag);
  for (const [k, v] of Object.entries(attrs)) if (v != null) node.setAttribute(k, String(v));
  return node;
}

/** An icon from the sprite in index.html. */
export function icon(name, cls = 'icon') {
  const s = svgEl('svg', { class: cls, 'aria-hidden': 'true', focusable: 'false' });
  s.append(svgEl('use', { href: `#i-${name}` }));
  return s;
}

export function clamp(v, lo, hi) { return Math.min(hi, Math.max(lo, v)); }

/** Delays fn until ms after the last call. flush() runs a pending call right away (e.g. before an action that must
 *  see the latest input), cancel() drops it. */
export function debounce(fn, ms) {
  let t = null;
  let pending = null;
  const run = () => {
    const args = pending;
    t = null;
    pending = null;
    fn(...args);
  };
  const d = (...args) => { pending = args; clearTimeout(t); t = setTimeout(run, ms); };
  d.cancel = () => { clearTimeout(t); t = null; pending = null; };
  d.flush = () => { if (t !== null) { clearTimeout(t); run(); } };
  return d;
}

// ------------------------------------------------------------------------------------------- numbers

const intFmt = new Intl.NumberFormat('en-US');
export const isNum = (v) => typeof v === 'number' && Number.isFinite(v);
export const fmtInt = (v) => (isNum(v) ? intFmt.format(Math.round(v)) : '–');

/** Significant-digit formatting without exponent noise for the usual magnitudes. */
export function fmtNum(v, sig = 4) {
  if (!isNum(v)) return '–';
  if (v === 0) return '0';
  const a = Math.abs(v);
  if (a >= 1e6 || a < 1e-3) return fmtSci(v, sig - 1);
  const digits = Math.max(0, sig - 1 - Math.floor(Math.log10(a)));
  return v.toFixed(Math.min(digits, 8));
}

const SUP = { '-': '⁻', 0: '⁰', 1: '¹', 2: '²', 3: '³', 4: '⁴', 5: '⁵', 6: '⁶', 7: '⁷', 8: '⁸', 9: '⁹' };
export const superscript = (n) => String(n).split('').map((c) => SUP[c] ?? c).join('');

/** 3.98 × 10⁻³ */
export function fmtSci(v, decimals = 2) {
  if (!isNum(v)) return '–';
  if (v === 0) return '0';
  const e = Math.floor(Math.log10(Math.abs(v)));
  let m = v / 10 ** e;
  let ee = e;
  if (Math.abs(Number(m.toFixed(decimals))) >= 10) { m /= 10; ee += 1; }
  return `${m.toFixed(decimals)} × 10${superscript(ee)}`;
}

export function fmtPow10(e) { return `10${superscript(e)}`; }

export function fmtSigned(v, digits = 1) {
  if (!isNum(v)) return '–';
  const s = v.toFixed(digits);
  return v > 0 ? `+${s}` : s.replace('-', '−');
}

export function fmtDuration(sec) {
  if (!isNum(sec)) return '–';
  if (sec < 60) return `${sec < 10 ? sec.toFixed(1) : Math.round(sec)} s`;
  const m = Math.floor(sec / 60);
  const s = Math.round(sec - m * 60);
  if (m < 60) return `${m} min ${String(s).padStart(2, '0')} s`;
  const h = Math.floor(m / 60);
  return `${h} h ${String(m - h * 60).padStart(2, '0')} min`;
}

const timeFmt = new Intl.DateTimeFormat(undefined, { hour: '2-digit', minute: '2-digit' });
const dateTimeFmt = new Intl.DateTimeFormat(undefined, { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' });
const fullFmt = new Intl.DateTimeFormat(undefined, { dateStyle: 'medium', timeStyle: 'medium' });

/** Local time of a server epoch (seconds); the date is shown unless it is today. */
export function fmtWhen(epochSec) {
  if (!isNum(epochSec)) return '–';
  const d = new Date(epochSec * 1000);
  const now = new Date();
  return d.toDateString() === now.toDateString() ? timeFmt.format(d) : dateTimeFmt.format(d);
}
export const fmtWhenFull = (epochSec) => (isNum(epochSec) ? fullFmt.format(new Date(epochSec * 1000)) : '–');

export const fmtGrid = (res) => (Array.isArray(res) && res.length === 3 ? res.join(' × ') : '–');

// ------------------------------------------------------------------------------------------- JSON

const isScalar = (v) => v === null || typeof v !== 'object';

function inline(v) {
  if (Array.isArray(v)) return `[${v.map(inline).join(', ')}]`;
  if (v && typeof v === 'object') {
    return `{${Object.entries(v).map(([k, x]) => `${JSON.stringify(k)}: ${inline(x)}`).join(', ')}}`;
  }
  return JSON.stringify(v) ?? 'null';
}

/**
 * Pretty-prints JSON compactly: arrays of scalars (coordinates, edges) and small flat objects stay on one line,
 * everything else is indented. Much easier to read in the editor than JSON.stringify(v, null, 2).
 */
export function formatJSON(value, indent = 2, width = 78) {
  const fmt = (v, level) => {
    if (isScalar(v)) return JSON.stringify(v) ?? 'null';
    const flat = Array.isArray(v)
      ? v.every(isScalar)
      : Object.values(v).every((x) => isScalar(x) || (Array.isArray(x) && x.every(isScalar)));
    const one = inline(v);
    if (flat && one.length + level * indent <= width) return one;
    const pad = ' '.repeat((level + 1) * indent);
    const end = ' '.repeat(level * indent);
    if (Array.isArray(v)) {
      if (!v.length) return '[]';
      return `[\n${v.map((x) => pad + fmt(x, level + 1)).join(',\n')}\n${end}]`;
    }
    const entries = Object.entries(v);
    if (!entries.length) return '{}';
    return `{\n${entries.map(([k, x]) => `${pad}${JSON.stringify(k)}: ${fmt(x, level + 1)}`).join(',\n')}\n${end}}`;
  };
  return `${fmt(value, 0)}\n`;
}

function lineColOf(text, pos) {
  let line = 1;
  let col = 1;
  for (let i = 0; i < pos && i < text.length; i++) {
    if (text[i] === '\n') { line++; col = 1; } else col++;
  }
  return { line, column: col };
}

/** JSON.parse with a readable error location: {ok, value} or {ok: false, message, line, column, position}. */
export function parseJSON(text) {
  if (!text.trim()) return { ok: false, message: 'The configuration is empty.' };
  try {
    return { ok: true, value: JSON.parse(text) };
  } catch (e) {
    const raw = String(e && e.message ? e.message : e);
    let position = null;
    let line = null;
    let column = null;
    let m = raw.match(/line (\d+) column (\d+)/i);
    if (m) { line = Number(m[1]); column = Number(m[2]); }
    m = raw.match(/position (\d+)/i);
    if (m) position = Number(m[1]);
    if (position != null && line == null) ({ line, column } = lineColOf(text, position));
    if (line != null && position == null) {
      const lines = text.split('\n');
      position = lines.slice(0, line - 1).reduce((s, l) => s + l.length + 1, 0) + (column - 1);
    }
    const message = raw.replace(/^JSON\.parse: /, '').replace(/\s*\(line \d+ column \d+\)/, '')
      .replace(/ in JSON at position \d+/, '').replace(/ of the JSON data$/, '');
    return { ok: false, message, line, column, position };
  }
}

/**
 * The JSON path an optimizer message starts with, e.g. "skeleton.edges[10] is a self-loop" -> ['skeleton', 'edges', 10]
 * or "'grid' must be an object" -> ['grid']. Null when the message does not name a configuration entry.
 */
export function pathOfMessage(message) {
  const m = String(message || '').match(/^'?([A-Za-z_]\w*(?:\.[A-Za-z_]\w*|\[\d+\])*)'?(?=[\s:.,=]|$)/);
  if (!m) return null;
  const path = [];
  for (const part of m[1].matchAll(/([A-Za-z_]\w*)|\[(\d+)\]/g)) path.push(part[1] !== undefined ? part[1] : Number(part[2]));
  // a lone word only counts when it is a top-level key of the configuration ("the fixed grid vertices ..." is not)
  const TOP = new Set(['name', 'description', 'grid', 'skeleton', 'boundary_conditions', 'optimization', 'output', 'paper_reference']);
  return path.length > 1 || TOP.has(path[0]) ? path : null;
}

/**
 * Character range {start, end, depth} of the value at `path` in a JSON text, or of the deepest existing parent when
 * the path does not exist (e.g. a missing key); `depth` is the number of path components found (0 = the whole
 * document). Null if the text does not parse. Used to select an entry the server complained about in the editor.
 */
export function locateJSONPath(text, path) {
  let i = 0;
  const n = text.length;
  const ws = () => { while (i < n && ' \t\r\n'.includes(text[i])) i++; };
  const fail = () => { throw new Error('bad JSON'); };
  const readString = () => {
    if (text[i] !== '"') fail();
    const s = i++;
    while (i < n && text[i] !== '"') i += text[i] === '\\' ? 2 : 1;
    if (i >= n) fail();
    i++;
    return JSON.parse(text.slice(s, i));
  };
  // walks the value starting at i; when `want` (the remaining path) is non-empty, descends into the matching child
  const value = (want) => {
    ws();
    const start = i;
    const c = text[i];
    let found = null;
    if (c === '{' || c === '[') {
      const isObj = c === '{';
      const close = isObj ? '}' : ']';
      i++;
      ws();
      let index = 0;
      if (text[i] === close) i++;
      else {
        for (;;) {
          let key = index;
          if (isObj) { ws(); key = readString(); ws(); if (text[i] !== ':') fail(); i++; }
          const child = want.length && want[0] === key ? value(want.slice(1)) : (value([]), null);
          if (child) found = child;
          index++;
          ws();
          if (text[i] === ',') { i++; continue; }
          if (text[i] !== close) fail();
          i++;
          break;
        }
      }
    } else if (c === '"') readString();
    else {
      while (i < n && !' \t\r\n,]}'.includes(text[i])) i++;
      if (i === start) fail();
    }
    return found ? { start: found.start, end: found.end, depth: found.depth + 1 } : { start, end: i, depth: 0 };
  };
  try { return value(path); } catch { return null; }
}

// ------------------------------------------------------------------------------------------- misc

/** localStorage wrapped in try/catch (it can be unavailable, e.g. in a private window). */
export const storage = {
  get(key, fallback = null) {
    try { const v = localStorage.getItem(key); return v == null ? fallback : v; } catch { return fallback; }
  },
  set(key, value) {
    try { if (value == null) localStorage.removeItem(key); else localStorage.setItem(key, value); } catch { /* ignore */ }
  },
};

export function downloadText(text, filename, type = 'application/json') {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const a = el('a', { href: url, download: filename });
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 2000);
}

export function downloadBlob(blob, filename) {
  const url = URL.createObjectURL(blob);
  const a = el('a', { href: url, download: filename });
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 2000);
}

// ------------------------------------------------------------------------------------------- toasts

const TOAST_ICON = { error: 'alert', ok: 'check', info: 'info' };

/** Shows a transient message in the bottom-right corner (errors stay longer and use role=alert). */
export function toast(message, kind = 'info', timeout) {
  const host = document.getElementById('toasts');
  if (!host) return;
  const close = el('button', { class: 'icon-btn', type: 'button', 'aria-label': 'Dismiss' }, icon('x'));
  const t = el('div', { class: `toast is-${kind}`, role: kind === 'error' ? 'alert' : 'status' },
    icon(TOAST_ICON[kind] || 'info'), el('div', { class: 'msg' }, message), close);
  const remove = () => t.remove();
  close.addEventListener('click', remove);
  host.append(t);
  while (host.children.length > 4) host.firstElementChild.remove();
  setTimeout(remove, timeout ?? (kind === 'error' ? 10000 : 4500));
}

/** Resolves true when the user confirms in the shared <dialog>. */
export function confirmDialog({ title, text, ok = 'Delete' }) {
  const dlg = document.getElementById('confirm-dialog');
  document.getElementById('confirm-title').textContent = title;
  document.getElementById('confirm-text').textContent = text;
  document.getElementById('confirm-ok').textContent = ok;
  return new Promise((resolve) => {
    const onClose = () => { dlg.removeEventListener('close', onClose); resolve(dlg.returnValue === 'ok'); };
    dlg.addEventListener('close', onClose);
    dlg.returnValue = '';
    dlg.showModal();
  });
}
