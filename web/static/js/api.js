// REST client for web/server.py (see docs/WEB_API.md). All URLs are relative so that the page also works
// behind a reverse proxy that mounts it under a path prefix; the proxy's auth cookie is sent automatically
// because the page and the API are same-origin.

export class ApiError extends Error {
  constructor(message, status = 0, detail = null) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.detail = detail;
  }
}

const enc = encodeURIComponent;

async function request(path, { method = 'GET', body, signal, as = 'json' } = {}) {
  const opts = { method, signal, credentials: 'same-origin', headers: {} };
  if (body !== undefined) {
    opts.headers['Content-Type'] = 'application/json';
    opts.body = JSON.stringify(body);
  }
  if (as === 'json') opts.headers.Accept = 'application/json';
  let res;
  try {
    res = await fetch(path, opts);
  } catch (e) {
    if (e && e.name === 'AbortError') throw e;
    throw new ApiError('Cannot reach the server (network error).', 0);
  }
  if (!res.ok) {
    let detail = null;
    try {
      const j = await res.json();
      detail = j && j.detail != null ? j.detail : null;
    } catch { /* not JSON (e.g. a proxy error page) */ }
    let msg;
    if (res.status === 401 || res.status === 403) msg = 'Not authorized: the access token is missing or expired. Reload the page from the instance portal.';
    else if (typeof detail === 'string') msg = detail;
    else if (detail != null) msg = JSON.stringify(detail);
    else msg = `HTTP ${res.status} ${res.statusText || ''}`.trim();
    throw new ApiError(msg, res.status, detail);
  }
  if (as === 'arraybuffer') return res.arrayBuffer();
  if (as === 'text') return res.text();
  try {
    return await res.json();
  } catch {
    throw new ApiError('The server sent an invalid response (not JSON).', res.status);
  }
}

export const api = {
  health: () => request('api/health'),
  examples: () => request('api/examples'),
  example: (id) => request(`api/examples/${enc(id)}`),
  validate: (config) => request('api/validate', { method: 'POST', body: config }),
  createRun: (config, threads) => request('api/runs', { method: 'POST', body: { config, threads } }),
  runs: () => request('api/runs'),
  run: (id, signal) => request(`api/runs/${enc(id)}`, { signal }),
  frame: (id, k, signal) => request(`api/runs/${enc(id)}/frames/${k}`, { signal }),
  theta: (id, k, signal) => request(`api/runs/${enc(id)}/theta/${k}`, { signal, as: 'arraybuffer' }),
  log: (id, tail = 200, signal) => request(`api/runs/${enc(id)}/log?tail=${tail}`, { signal }),
  cancel: (id) => request(`api/runs/${enc(id)}/cancel`, { method: 'POST' }),
  remove: (id) => request(`api/runs/${enc(id)}`, { method: 'DELETE' }),
  inputConfig: (id) => request(`api/runs/${enc(id)}/files/input.json`),
  fileUrl: (id, name) => `api/runs/${enc(id)}/files/${enc(name)}`,
  eventsUrl: (id, since) => `api/runs/${enc(id)}/events?since=${since}`,
};

export const TERMINAL = new Set(['completed', 'failed', 'cancelled', 'interrupted']);
export const isTerminal = (state) => TERMINAL.has(state);
