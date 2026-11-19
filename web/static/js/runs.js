// Sidebar list of runs (GET api/runs, newest first). Rows are re-rendered only when their data changed, so the
// polling never steals focus or resets hover states.

import { el, icon, fmtNum, fmtWhen, fmtWhenFull, fmtDuration, fmtGrid, fmtInt } from './util.js';
import { stateChip, methodLabel } from './status.js';
import { isTerminal } from './api.js';

export class RunsList {
  constructor(listEl, emptyEl, countEl, { onOpen, onCancel, onDelete }) {
    this.list = listEl;
    this.emptyEl = emptyEl;
    this.countEl = countEl;
    this.h = { onOpen, onCancel, onDelete };
    this.rows = new Map(); // id -> {li, key}
    this.current = null;
    this.revealPending = null; // id of an opened run whose row still has to be scrolled into the list's view
  }

  setCurrent(id) {
    if (id !== this.current) this.revealPending = id;
    this.current = id;
    for (const [rid, row] of this.rows) {
      row.li.classList.toggle('is-current', rid === id);
      row.open.setAttribute('aria-current', rid === id ? 'true' : 'false');
    }
    this.revealCurrent();
  }

  /**
   * Scrolls the list (its own scroll area, never the page) so that the row of the open run is visible, e.g. for a
   * run opened from a link. Once per opened run (also when its row appears with a later poll): the polling must not
   * fight a user who scrolls the list.
   */
  revealCurrent() {
    const id = this.revealPending;
    const row = id ? this.rows.get(id) : null;
    if (!row) return;
    this.revealPending = null;
    const list = this.list;
    if (list.scrollHeight <= list.clientHeight + 1) return;
    const top = row.li.offsetTop; // the list is the rows' offset parent (position: relative in app.css)
    const bottom = top + row.li.offsetHeight;
    if (top < list.scrollTop + 4) list.scrollTop = Math.max(0, top - 6);
    else if (bottom > list.scrollTop + list.clientHeight - 4) list.scrollTop = bottom - list.clientHeight + 6;
  }

  update(runs) {
    this.emptyEl.hidden = runs.length > 0;
    this.countEl.textContent = runs.length ? `(${runs.length})` : '';
    const seen = new Set();
    let prev = null;
    for (const r of runs) {
      seen.add(r.id);
      const key = JSON.stringify([r.state, r.iteration, r.compliance, r.queue_position, r.error, r.elapsed, r.max_iterations]);
      let row = this.rows.get(r.id);
      if (!row || row.key !== key) {
        const fresh = this.renderRow(r);
        if (row) row.li.replaceWith(fresh.li);
        row = { ...fresh, key };
        this.rows.set(r.id, row);
      }
      // keep the server order (newest first)
      const want = prev ? prev.nextSibling : this.list.firstChild;
      if (want !== row.li) this.list.insertBefore(row.li, want);
      prev = row.li;
    }
    for (const [id, row] of this.rows) {
      if (!seen.has(id)) { row.li.remove(); this.rows.delete(id); }
    }
    this.setCurrent(this.current);
  }

  renderRow(r) {
    const line2 = [methodLabel(r.method), r.resolution ? fmtGrid(r.resolution) : null,
      r.num_voxels ? `${fmtInt(r.num_voxels)} voxels` : null, fmtWhen(r.created)].filter(Boolean).join(' · ');
    let line3;
    if (r.state === 'failed' || r.state === 'interrupted') {
      line3 = el('div', { class: 'run-line3 is-error' }, r.error || 'stopped without a result');
    } else if (Number.isFinite(r.compliance)) {
      const parts = [`E ${fmtNum(r.compliance, 4)}`];
      if (Number.isInteger(r.iteration)) parts.push(`${r.iteration}${r.max_iterations ? ` / ${r.max_iterations}` : ''} it`);
      if (Number.isFinite(r.elapsed)) parts.push(fmtDuration(r.elapsed));
      line3 = el('div', { class: 'run-line3' }, parts.join(' · '));
    } else if (r.state === 'queued') {
      line3 = el('div', { class: 'run-line3' }, `${r.max_iterations ?? '?'} iterations, ${r.threads ? `${r.threads} threads` : 'all cores'}`);
    } else if (r.state === 'running') {
      line3 = el('div', { class: 'run-line3' }, 'starting…');
    } else {
      line3 = el('div', { class: 'run-line3' }, 'no iterations recorded');
    }
    const open = el('button', {
      class: 'run-open', type: 'button', title: `${r.name || r.id}\n${r.id}\ncreated ${fmtWhenFull(r.created)}`,
    },
    el('div', { class: 'run-line1' }, el('span', { class: 'run-name' }, r.name || r.id), stateChip(r)),
    el('div', { class: 'run-line2' }, line2),
    line3);
    open.addEventListener('click', () => this.h.onOpen(r.id));

    const actions = el('div', { class: 'run-actions' });
    if (!isTerminal(r.state)) {
      const b = el('button', { class: 'icon-btn danger', type: 'button', 'aria-label': `Cancel run ${r.name || r.id}`, title: 'Cancel this run' }, icon('stop'));
      b.addEventListener('click', (e) => { e.stopPropagation(); this.h.onCancel(r); });
      actions.append(b);
    } else {
      const b = el('button', { class: 'icon-btn danger', type: 'button', 'aria-label': `Delete run ${r.name || r.id}`, title: 'Delete this run' }, icon('trash'));
      b.addEventListener('click', (e) => { e.stopPropagation(); this.h.onDelete(r); });
      actions.append(b);
    }
    const li = el('li', { class: 'run-row', 'data-id': r.id }, open, actions);
    return { li, open };
  }
}
