// Light / dark theme: follows the OS setting (prefers-color-scheme) until the user picks one with the toggle.
// The explicit choice is stored per browser; choosing the theme the OS already uses goes back to "automatic".

import { storage } from './util.js';

const KEY = 'msd.theme';

export function initTheme(button, onChange) {
  const mq = window.matchMedia('(prefers-color-scheme: dark)');
  let mode = storage.get(KEY, 'auto');
  if (mode !== 'light' && mode !== 'dark') mode = 'auto';

  const effective = () => (mode === 'auto' ? (mq.matches ? 'dark' : 'light') : mode);

  const apply = () => {
    const root = document.documentElement;
    if (mode === 'auto') root.removeAttribute('data-theme');
    else root.setAttribute('data-theme', mode);
    const eff = effective();
    const label = `Color theme: ${eff}${mode === 'auto' ? ' (automatic)' : ''}. Switch to ${eff === 'dark' ? 'light' : 'dark'}.`;
    button.title = label;
    button.setAttribute('aria-label', label);
    const use = button.querySelector('use');
    if (use) use.setAttribute('href', eff === 'dark' ? '#i-moon' : '#i-sun');
    onChange?.(eff);
  };

  button.addEventListener('click', () => {
    const next = effective() === 'dark' ? 'light' : 'dark';
    mode = next === (mq.matches ? 'dark' : 'light') ? 'auto' : next;
    storage.set(KEY, mode === 'auto' ? null : mode);
    apply();
  });
  mq.addEventListener('change', () => { if (mode === 'auto') apply(); });
  apply();
}
