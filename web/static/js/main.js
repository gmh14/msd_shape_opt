// Entry point: wires the sidebar (new run, run list), the main workspace and the hash routes
//   #/run/<id>   an open run          #/preview   the last validated configuration          (empty)   no selection

import { api, isTerminal } from './api.js';
import { NewRunPanel } from './newrun.js';
import { RunsList } from './runs.js';
import { Workspace } from './workspace.js';
import { initTheme } from './theme.js';
import { $, toast, confirmDialog, fmtNum } from './util.js';

// unexpected errors must be visible, never only in the console
window.addEventListener('error', (e) => {
  if (e && e.message) toast(`Unexpected error: ${e.message}`, 'error');
});
window.addEventListener('unhandledrejection', (e) => {
  const r = e.reason;
  if (r && r.name === 'AbortError') { e.preventDefault(); return; }
  toast(`Unexpected error: ${r && r.message ? r.message : String(r)}`, 'error');
});

const RUN_ROUTE = /^#\/run\/([A-Za-z0-9][A-Za-z0-9._-]{0,127})$/;
const narrow = window.matchMedia('(max-width: 860px)'); // single column: the sidebar sits above the run view
const state = { runs: null, health: null, runsError: false, booting: true };

// The page restores its own scroll position: the content above the run view (examples, editor, run list) is built
// asynchronously, so a position restored by the browser on reload would point into the half-built sidebar.
if ('scrollRestoration' in history) history.scrollRestoration = 'manual';

function revealWorkspace() {
  // single-column layouts: bring the run view into sight after picking something in the sidebar. During startup
  // start() reveals it once the sidebar is complete (pinWorkspace).
  if (!narrow.matches || state.booting) return;
  requestAnimationFrame(() => $('#main').scrollIntoView({ behavior: 'smooth', block: 'start' }));
}

/**
 * Single-column layouts at startup (a shared #/run/<id> link, a reload): scroll the run view to the top of the
 * window and keep it there while the sidebar above it still changes height (fonts, the example description, the run
 * list), until the user scrolls, taps or types, or the layout has been quiet for a while.
 */
function pinWorkspace() {
  if (!narrow.matches) return;
  const main = $('#main');
  const go = () => window.scrollTo(0, Math.max(0, main.getBoundingClientRect().top + window.scrollY));
  go();
  const USER = ['wheel', 'touchstart', 'pointerdown', 'keydown'];
  const ro = new ResizeObserver(() => { go(); quiet(); });
  let timer = null;
  const stop = () => {
    ro.disconnect();
    clearTimeout(timer);
    for (const t of USER) window.removeEventListener(t, stop, true);
  };
  const quiet = () => { clearTimeout(timer); timer = setTimeout(stop, 1500); };
  for (const t of USER) window.addEventListener(t, stop, { capture: true, passive: true });
  ro.observe($('.sidebar'));
  quiet();
}

const workspace = new Workspace({
  onRunConfig: () => newRun.run(),
  onShowConfigError: () => newRun.showErrorInEditor(),
  onEditConfig: (cfg, label) => newRun.setConfig(cfg, label),
  onClosePreview: () => { location.hash = ''; },
  onRunsChanged: () => refreshRuns(),
  onMissing: () => { if (RUN_ROUTE.test(location.hash)) location.hash = ''; refreshRuns(); },
  onCancel: (r) => cancelRun(r),
  onDelete: (r) => deleteRun(r),
});

const newRun = new NewRunPanel({
  onPreview: (p) => {
    if (location.hash !== '#/preview') history.pushState(null, '', '#/preview');
    runsList.setCurrent(null);
    workspace.showPreview(p);
    revealWorkspace();
  },
  onCreated: (res) => {
    refreshRuns();
    location.hash = `#/run/${res.id}`;
    // two columns: the sidebar scrolls on its own; bring the run list (new run on top) back into view. The quick
    // settings and the Run button stay on screen below it for a variant of the same problem.
    const sidebar = $('.sidebar');
    if (!narrow.matches && sidebar.scrollTop > 0) sidebar.scrollTo({ top: 0, behavior: 'smooth' });
  },
  onEdited: () => workspace.markPreviewStale(),
  // a preview of an earlier configuration must not pass for the one that just failed validation
  onInvalid: (message, canLocate) => workspace.markPreviewInvalid(message, canLocate),
});

const runsList = new RunsList($('#run-list'), $('#runs-empty'), $('#runs-count'), {
  onOpen: (id) => {
    if (location.hash === `#/run/${id}`) { route(); revealWorkspace(); return; }
    location.hash = `#/run/${id}`;
  },
  onCancel: (r) => cancelRun(r),
  onDelete: (r) => deleteRun(r),
});

function route() {
  const m = location.hash.match(RUN_ROUTE);
  if (m) {
    runsList.setCurrent(m[1]);
    workspace.openRun(m[1]);
    revealWorkspace();
    return;
  }
  if (location.hash === '#/preview' && workspace.preview) return;
  runsList.setCurrent(null);
  workspace.showEmpty();
}
window.addEventListener('hashchange', route);

// ---------------------------------------------------------------------------------------- run list polling

let pollTimer = null;

async function refreshRuns() {
  try {
    const runs = await api.runs();
    if (state.runsError) toast('The server is reachable again.', 'ok');
    state.runsError = false;
    notifyTransitions(state.runs, runs);
    state.runs = runs;
    runsList.update(runs);
    $('#empty-has-runs').hidden = runs.length === 0;
    $('#empty-no-runs').hidden = runs.length > 0;
    renderServerInfo();
    // the open run was deleted elsewhere (another tab or user)
    const open = workspace.session;
    if (open && open.status && !runs.some((r) => r.id === open.id)) {
      toast(`Run ${open.status.name || open.id} was deleted.`, 'info');
      if (RUN_ROUTE.test(location.hash)) location.hash = '';
    }
  } catch (e) {
    if (!state.runsError) toast(`Could not load the run list: ${e.message}`, 'error');
    state.runsError = true;
    renderServerInfo(e);
  }
}

function schedulePoll() {
  clearTimeout(pollTimer);
  pollTimer = setTimeout(async () => {
    await refreshRuns();
    schedulePoll();
  }, document.hidden ? 15000 : 3000);
}

document.addEventListener('visibilitychange', () => {
  if (!document.hidden) { refreshRuns(); schedulePoll(); }
});

/** Toasts for runs that finished in the background (the open run reports its own end). */
function notifyTransitions(before, after) {
  if (!before) return;
  const old = new Map(before.map((r) => [r.id, r.state]));
  const open = workspace.session && workspace.session.id;
  for (const r of after) {
    const was = old.get(r.id);
    if (!was || isTerminal(was) || !isTerminal(r.state) || r.id === open) continue;
    const name = r.name || r.id;
    if (r.state === 'completed') toast(`Run ${name} completed: compliance ${fmtNum(r.compliance, 4)}.`, 'ok');
    else if (r.state === 'failed') toast(`Run ${name} failed: ${r.error || 'see its log'}`, 'error');
    else if (r.state === 'interrupted') toast(`Run ${name} was interrupted.`, 'error');
  }
}

function renderServerInfo(err) {
  const box = $('#server-info');
  if (err) {
    // a 401/403 comes from the authenticating proxy: the server is fine, the token is not
    box.textContent = err.status === 401 || err.status === 403 ? `${err.message} Retrying…` : `Server unreachable (${err.message}). Retrying…`;
    return;
  }
  const runs = state.runs || [];
  const running = runs.filter((r) => r.state === 'running').length;
  const queued = runs.filter((r) => r.state === 'queued').length;
  const h = state.health;
  const parts = [];
  if (h) parts.push(`${h.cpu_count} cores`);
  parts.push(`${running} running`, `${queued} queued`);
  box.textContent = `Server: ${parts.join(' · ')}`;
  box.title = h ? `The server runs up to ${h.max_concurrent} optimization${h.max_concurrent > 1 ? 's' : ''} at a time; further runs wait in the queue.` : '';
}

// ---------------------------------------------------------------------------------------- actions

async function cancelRun(r) {
  try {
    const res = await api.cancel(r.id);
    const name = r.name || r.id;
    if (res.result === 'cancelling' || res.result === 'cancelled') toast(`Cancelling ${name}…`, 'info');
    else toast(`${name} has already finished (${res.result}).`, 'info');
  } catch (e) {
    toast(`Could not cancel the run: ${e.message}`, 'error');
  }
  refreshRuns();
}

async function deleteRun(r) {
  const name = r.name || r.id;
  const ok = await confirmDialog({
    title: 'Delete this run?',
    text: `"${name}" (${r.id}) and all its files (frames, logs, results) will be removed from the server. This cannot be undone.`,
    ok: 'Delete',
  });
  if (!ok) return;
  try {
    await api.remove(r.id);
    toast(`Deleted ${name}.`, 'ok');
    if (workspace.session && workspace.session.id === r.id) location.hash = '';
  } catch (e) {
    toast(`Could not delete the run: ${e.message}`, 'error');
  }
  refreshRuns();
}

$('#runs-refresh').addEventListener('click', () => refreshRuns());

// ---------------------------------------------------------------------------------------- start

async function start() {
  initTheme($('#theme-toggle'), () => workspace.onThemeChange());
  // single column + a run link: the New run panel starts collapsed, so that less of the sidebar sits above the run
  if (narrow.matches && RUN_ROUTE.test(location.hash)) $('#newrun-section').open = false;
  try {
    state.health = await api.health();
    if (!state.health.binary_exists) {
      const b = $('#server-banner');
      b.textContent = `The optimizer binary was not found on the server (${state.health.binary}). Build the project before starting runs.`;
      b.hidden = false;
    }
  } catch (e) {
    const b = $('#server-banner');
    b.textContent = `Cannot reach the server: ${e.message}`;
    b.hidden = false;
  }
  route(); // opens a linked run right away; the scrolling waits for the sidebar (revealWorkspace is off while booting)
  try {
    await Promise.all([newRun.init(state.health), refreshRuns()]);
  } finally {
    state.booting = false;
  }
  if (workspace.mode !== 'empty') pinWorkspace();
  schedulePoll();
}

start();
