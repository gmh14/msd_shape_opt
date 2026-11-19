// Run-state chip (icon + label; the status color is only on the icon) shared by the run list and the run view.

import { el, icon } from './util.js';

const LABEL = {
  queued: 'Queued', running: 'Running', completed: 'Completed', failed: 'Failed',
  cancelled: 'Cancelled', interrupted: 'Interrupted', preview: 'Preview', loading: 'Loading',
};
const ICON = {
  queued: 'clock', completed: 'check', failed: 'alert', cancelled: 'slash',
  interrupted: 'alert', preview: 'eye', loading: 'clock',
};

/** status: a status object (or {state}); extra details: queue position, iteration / max. */
export function stateChip(status) {
  const state = (status && status.state) || 'loading';
  const chip = el('span', { class: `chip st-${state}` });
  if (state === 'running') chip.append(el('span', { class: 'dot', 'aria-hidden': 'true' }));
  else chip.append(icon(ICON[state] || 'info'));
  chip.append(el('span', {}, LABEL[state] || state));
  let extra = null;
  if (state === 'running') {
    const it = Number.isInteger(status.iteration) ? status.iteration : 0;
    extra = status.max_iterations ? `${it} / ${status.max_iterations}` : `iteration ${it}`;
  } else if (state === 'queued' && Number.isInteger(status.queue_position)) {
    extra = `#${status.queue_position + 1} in line`;
  }
  if (extra) chip.append(el('span', { class: 'chip-extra' }, extra));
  chip.title = [LABEL[state] || state, extra].filter(Boolean).join(' · ');
  return chip;
}

export const methodLabel = (m) => (m === 'ma' ? 'MA' : m === 'msd' ? 'MSD' : (m || '?').toUpperCase());
export const methodLong = (m) => (m === 'ma' ? 'MA (medial-axis baseline)' : 'MSD (generalized enveloping primitives)');
