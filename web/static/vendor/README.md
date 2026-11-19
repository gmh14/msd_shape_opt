# Vendored libraries

The page makes no runtime requests to external hosts; these files are served from here.

| file | package | source URL | sha256 |
|---|---|---|---|
| `three-0.170.0/three.module.min.js` | three.js r170 (MIT) | https://cdn.jsdelivr.net/npm/three@0.170.0/build/three.module.min.js | `08fd7545d13d2c7fb65ab691530a802dafefd638596501854f267d0fb13c39e7` |
| `three-0.170.0/OrbitControls.js` | three.js r170 addon (MIT) | https://cdn.jsdelivr.net/npm/three@0.170.0/examples/jsm/controls/OrbitControls.js | `80efaadea4f8a636a65fb0bd08bfef62f3d93a0bb94e2e7500f23176c5c07f4e` |

Both files were checked against the hashes published by the jsDelivr data API
(`https://data.jsdelivr.com/v1/packages/npm/three@0.170.0?structure=flat`) when they were downloaded.
`index.html` maps the bare specifier `three` to `three-0.170.0/three.module.min.js` with an import map
(`OrbitControls.js` imports from `'three'`).

The charts are drawn by `js/charts.js` (plain SVG); no chart library is used.
