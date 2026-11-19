// 3D view of a problem / an optimization frame (three.js r170).
//
// World coordinates are the ones of the run files (world = grid * cell_size). Everything that belongs to one
// problem lives in `problem.group` and is disposed by clearProblem(); the renderer, the camera and the lights
// are kept for the lifetime of the page. Rendering is on demand (controls moved, frame or layer changed).

import * as THREE from 'three';
import { OrbitControls } from 'three/addons/OrbitControls.js';

const AXES = [new THREE.Vector3(1, 0, 0), new THREE.Vector3(0, 1, 0), new THREE.Vector3(0, 0, 1)];
const Y_UP = new THREE.Vector3(0, 1, 0);
const AXIS_COLORS = ['#e34948', '#1f9d3a', '#2a78d6'];
const AXIS_NAMES = ['x', 'y', 'z'];
const MAX_ARROWS = 160;

function cssVar(name, fallback) {
  const v = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  return v || fallback;
}

/** Ghost copy of an instanced mesh that is drawn only where the original is hidden behind other geometry. */
function xrayGhost(mesh, color, opacity) {
  const mat = new THREE.MeshBasicMaterial({
    color, transparent: true, opacity, depthWrite: false, depthFunc: THREE.GreaterDepth,
    // pull the ghost slightly towards the camera so that it never passes the depth test on visible parts
    polygonOffset: true, polygonOffsetFactor: -1, polygonOffsetUnits: -4,
  });
  const g = new THREE.InstancedMesh(mesh.geometry, mat, mesh.instanceMatrix.count);
  g.instanceMatrix = mesh.instanceMatrix; // shared: updating the original updates the ghost
  g.count = mesh.count;
  g.frustumCulled = false;
  g.renderOrder = 10;
  return g;
}

/**
 * Groups the loaded grid vertices into connected patches (26-neighbourhood, similar force direction).
 * Returns [{center (grid coordinates), force (sum), vertices}].
 */
function loadPatches(loads) {
  const key = (x, y, z) => `${x},${y},${z}`;
  const index = new Map(loads.map((l, i) => [key(...l.vertex), i]));
  const unit = loads.map((l) => {
    const f = l.force || [0, 0, 0];
    const n = Math.hypot(...f) || 1;
    return [f[0] / n, f[1] / n, f[2] / n];
  });
  const parent = loads.map((_, i) => i);
  const find = (i) => {
    while (parent[i] !== i) { parent[i] = parent[parent[i]]; i = parent[i]; }
    return i;
  };
  loads.forEach((l, i) => {
    const [x, y, z] = l.vertex;
    for (let dx = -1; dx <= 1; dx++) {
      for (let dy = -1; dy <= 1; dy++) {
        for (let dz = -1; dz <= 1; dz++) {
          const j = index.get(key(x + dx, y + dy, z + dz));
          if (j === undefined || j === i) continue;
          const u = unit[i];
          const v = unit[j];
          if (u[0] * v[0] + u[1] * v[1] + u[2] * v[2] > 0.9) parent[find(i)] = find(j);
        }
      }
    }
  });
  const groups = new Map();
  loads.forEach((l, i) => {
    const r = find(i);
    let g = groups.get(r);
    if (!g) groups.set(r, (g = { sum: [0, 0, 0], force: [0, 0, 0], vertices: [] }));
    const f = l.force || [0, 0, 0];
    for (let d = 0; d < 3; d++) { g.sum[d] += l.vertex[d]; g.force[d] += f[d]; }
    g.vertices.push(l.vertex);
  });
  return [...groups.values()].map((g) => ({ center: g.sum.map((v) => v / g.vertices.length), force: g.force, vertices: g.vertices }));
}

export class Viewer {
  constructor(container, { gizmo = null } = {}) {
    this.container = container;
    this.gizmo = gizmo;
    this.layers = { voxels: true, primitives: false, skeleton: true, initial: false, bcs: true, box: true };
    this.clip = true;
    this.threshold = 0.5;
    this.theta = null;
    this.problem = null;
    this.onInfo = null;
    this.needsRender = true;
    this.available = false;
    this.edgeUniform = { value: 0.3 };

    try {
      this.renderer = new THREE.WebGLRenderer({ antialias: true, powerPreference: 'high-performance' });
    } catch (e) {
      this.error = e;
      return;
    }
    this.available = true;
    const r = this.renderer;
    r.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
    r.localClippingEnabled = true;
    r.domElement.setAttribute('aria-label', '3D view. Drag to rotate, scroll or pinch to zoom, right-drag or two-finger drag to pan.');
    r.domElement.setAttribute('role', 'img');
    container.prepend(r.domElement);
    r.domElement.addEventListener('webglcontextlost', (e) => { e.preventDefault(); this.onContextLost?.(); });
    r.domElement.addEventListener('webglcontextrestored', () => { this.requestRender(); this.onContextRestored?.(); });

    this.scene = new THREE.Scene();
    this.camera = new THREE.PerspectiveCamera(30, 1, 0.001, 100);
    this.camera.position.set(1, 1, 1);
    this.scene.add(this.camera);
    // soft fill from the sky + a key light attached to the camera (upper left), so the shading stays readable
    // from every direction while orbiting
    this.hemi = new THREE.HemisphereLight(0xffffff, 0x77746f, 1.35);
    this.scene.add(this.hemi);
    this.key = new THREE.DirectionalLight(0xffffff, 1.75);
    this.key.position.set(-0.55, 0.85, 1.0);
    this.key.target.position.set(0, 0, -1);
    this.camera.add(this.key, this.key.target);

    this.world = new THREE.Group();
    this.scene.add(this.world);
    this.controls = null;

    this.ro = new ResizeObserver(() => this.resize());
    this.ro.observe(container);
    this.resize();
    this.applyTheme();
    this.buildGizmo();

    const loop = () => {
      this.raf = requestAnimationFrame(loop);
      const moved = this.controls ? this.controls.update() : false;
      if (moved || this.needsRender) {
        this.needsRender = false;
        this.render();
      }
    };
    this.raf = requestAnimationFrame(loop);
  }

  requestRender() { this.needsRender = true; }

  resize() {
    if (!this.available) return;
    const w = Math.max(1, this.container.clientWidth);
    const h = Math.max(1, this.container.clientHeight);
    if (w === this.size?.w && h === this.size?.h) return;
    this.size = { w, h };
    this.renderer.setSize(w, h, false);
    this.camera.aspect = w / h;
    this.camera.updateProjectionMatrix();
    // keep a preset view framed while the layout changes, until the user moves the camera
    if (this.autoView && this.problem && w > 1 && h > 1) this.setView(this.autoView);
    this.requestRender();
  }

  render() {
    this.renderer.render(this.scene, this.camera);
    this.updateGizmo();
  }

  // ---------------------------------------------------------------------------------------- theme

  applyTheme() {
    if (!this.available) return;
    this.colors = {
      bg: cssVar('--viewer-bg', '#f7f7f5'),
      box: cssVar('--viewer-box', '#9e9d97'),
      voxel: cssVar('--voxel', '#d4d4d0'),
      vertex: cssVar('--skel-vertex', '#d63b3b'),
      edge: cssVar('--skel-edge', '#262624'),
      bc: cssVar('--bc', '#eb6834'),
      primitive: cssVar('--primitive', '#2a78d6'),
      initial: cssVar('--initial', '#8f8e88'),
    };
    this.edgeUniform.value = Number(cssVar('--voxel-edge', '0.3')) || 0.3;
    this.renderer.setClearColor(this.colors.bg, 1);
    const P = this.problem;
    if (P) {
      for (const [mat, key] of P.themed) mat.color.set(this.colors[key]);
    }
    this.requestRender();
  }

  // ---------------------------------------------------------------------------------------- problem

  /** Builds the static scene of a problem from meta.json (or the meta returned by api/validate). */
  setProblem(meta, { keepCamera = false } = {}) {
    if (!this.available) return;
    const res = (meta.grid?.resolution || [1, 1, 1]).map(Number);
    const sameDomain = this.problem && this.problem.res.every((v, i) => v === res[i]);
    const keepView = keepCamera && sameDomain;
    const savedPos = keepView ? this.camera.position.clone() : null;
    const savedTarget = keepView && this.controls ? this.controls.target.clone() : null;
    this.clearProblem();

    const c = Number(meta.grid?.cell_size) || 0.01;
    const size = new THREE.Vector3(res[0] * c, res[1] * c, res[2] * c);
    const tmin = Number(meta.optimization?.theta_min);
    const P = {
      meta, res, c, size,
      thetaMin: tmin >= 0 && tmin < 0.5 ? tmin : 0.01, // density of an empty voxel (decodes the theta bytes)
      center: size.clone().multiplyScalar(0.5),
      maxDim: Math.max(size.x, size.y, size.z),
      numVoxels: res[0] * res[1] * res[2],
      group: new THREE.Group(),
      themed: [], // [material, color key] pairs recolored by applyTheme()
      // marker size: one voxel, growing mildly for large grids so that markers stay visible
      mark: c * Math.max(1, Math.max(...res) / 64),
      extraPoints: [], // points outside the domain box that the camera fit must include (load arrows)
    };
    P.up = this.upAxis(meta);
    P.planes = [
      new THREE.Plane(new THREE.Vector3(1, 0, 0), 0), new THREE.Plane(new THREE.Vector3(-1, 0, 0), size.x),
      new THREE.Plane(new THREE.Vector3(0, 1, 0), 0), new THREE.Plane(new THREE.Vector3(0, -1, 0), size.y),
      new THREE.Plane(new THREE.Vector3(0, 0, 1), 0), new THREE.Plane(new THREE.Vector3(0, 0, -1), size.z),
    ];
    this.problem = P;
    this.world.add(P.group);

    this.buildBox(P);
    this.buildVoxels(P);
    this.buildPrimitives(P);
    this.buildSkeleton(P);
    this.buildInitialSkeleton(P);
    this.buildBoundaryConditions(P);
    this.setupControls(P);
    if (keepView) {
      this.camera.position.copy(savedPos);
      this.controls.target.copy(savedTarget);
      this.controls.update();
    } else {
      this.setView('default');
    }
    this.applyLayers();
    this.requestRender();
  }

  clearProblem() {
    const P = this.problem;
    if (!P) return;
    this.world.remove(P.group);
    const done = new Set();
    P.group.traverse((o) => {
      for (const res of [o.geometry, ...(Array.isArray(o.material) ? o.material : [o.material])]) {
        if (res && !done.has(res)) { done.add(res); res.dispose(); }
      }
      if (o.isInstancedMesh) o.dispose(); // frees the instance buffers on the GPU
    });
    this.problem = null;
    this.theta = null;
    this.requestRender();
  }

  /** The screen "up" is the direction opposite to the total load (snapped to the dominant axis). */
  upAxis(meta) {
    const f = meta.boundary_conditions?.total_force || [0, 0, 0];
    let k = 1;
    let best = 0;
    for (let i = 0; i < 3; i++) {
      if (Math.abs(f[i]) > best * (1 + 1e-9)) { best = Math.abs(f[i]); k = i; }
    }
    const sign = best > 0 ? -Math.sign(f[k]) : 1;
    return { k, vec: AXES[k].clone().multiplyScalar(sign) };
  }

  setupControls(P) {
    // OrbitControls captures camera.up as its orbit axis when it is created: recreate it when "up" changes
    if (!this.controls || !this.camera.up.equals(P.up.vec)) {
      if (this.controls) this.controls.dispose();
      this.camera.up.copy(P.up.vec);
      this.controls = new OrbitControls(this.camera, this.renderer.domElement);
      this.controls.enableDamping = true;
      this.controls.dampingFactor = 0.14;
      this.controls.screenSpacePanning = true;
      this.controls.rotateSpeed = 0.9;
      this.controls.addEventListener('change', () => this.requestRender());
      this.controls.addEventListener('start', () => { this.autoView = null; });
    }
    this.controls.target.copy(P.center);
    this.controls.minDistance = P.maxDim * 0.05;
    this.controls.maxDistance = P.maxDim * 25;
    this.camera.near = P.maxDim / 400;
    this.camera.far = P.maxDim * 60;
    this.camera.updateProjectionMatrix();
  }

  buildBox(P) {
    const geo = new THREE.EdgesGeometry(new THREE.BoxGeometry(P.size.x, P.size.y, P.size.z));
    const mat = new THREE.LineBasicMaterial({ color: this.colors.box });
    P.themed.push([mat, 'box']);
    P.box = new THREE.LineSegments(geo, mat);
    P.box.position.copy(P.center);
    P.group.add(P.box);
  }

  buildVoxels(P) {
    const geo = new THREE.BoxGeometry(P.c, P.c, P.c);
    const mat = new THREE.MeshLambertMaterial({ color: this.colors.voxel });
    P.themed.push([mat, 'voxel']);
    // thin, anti-aliased darkening along the edges of every voxel face, so that individual voxels read as
    // voxels; it fades out when a voxel covers only a few pixels
    mat.defines = { USE_UV: '' };
    mat.onBeforeCompile = (shader) => {
      shader.uniforms.uEdge = this.edgeUniform;
      shader.fragmentShader = shader.fragmentShader
        .replace('#include <common>', '#include <common>\nuniform float uEdge;')
        .replace('#include <color_fragment>', `#include <color_fragment>
          {
            vec2 q = min(vUv, 1.0 - vUv);
            float d = min(q.x, q.y);
            float w = fwidth(d);
            float line = 1.0 - smoothstep(0.35 * w, 1.35 * w, d);
            line *= 1.0 - smoothstep(0.05, 0.16, w);
            diffuseColor.rgb *= 1.0 - uEdge * line;
          }`);
    };
    mat.customProgramCacheKey = () => 'msd-voxel-edges';
    const mesh = new THREE.InstancedMesh(geo, mat, Math.max(1, P.numVoxels));
    mesh.instanceMatrix.setUsage(THREE.DynamicDrawUsage);
    const a = mesh.instanceMatrix.array;
    for (let i = 0, n = mesh.instanceMatrix.count; i < n; i++) { // identity; only translations change later
      const o = i * 16;
      a[o] = 1; a[o + 5] = 1; a[o + 10] = 1; a[o + 15] = 1;
    }
    mesh.count = 0;
    mesh.frustumCulled = false;
    P.voxels = mesh;
    P.group.add(mesh);
  }

  buildPrimitives(P) {
    const pr = P.meta.primitives;
    if (!pr || !pr.sphere_template || !pr.cylinder_template) return;
    const nV = pr.num_vertex_primitives | 0;
    const nE = pr.num_edge_primitives | 0;
    const sT = pr.sphere_template;
    const cT = pr.cylinder_template;
    const total = nV * sT.num_vertices + nE * cT.num_vertices;
    const index = [];
    let off = 0;
    for (let p = 0; p < nV + nE; p++) {
      const t = p < nV ? sT : cT;
      for (const [a, b, c] of t.triangles) index.push(off + a, off + b, off + c);
      off += t.num_vertices;
    }
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.BufferAttribute(new Float32Array(total * 3), 3).setUsage(THREE.DynamicDrawUsage));
    geo.setAttribute('normal', new THREE.BufferAttribute(new Float32Array(total * 3), 3));
    geo.setIndex(index);
    const mat = new THREE.MeshPhongMaterial({
      color: this.colors.primitive, transparent: true, opacity: 0.2, depthWrite: false,
      side: THREE.DoubleSide, flatShading: true, shininess: 10,
    });
    const lineMat = new THREE.LineBasicMaterial({ color: this.colors.primitive, transparent: true, opacity: 0.65 });
    P.themed.push([mat, 'primitive'], [lineMat, 'primitive']);
    const mesh = new THREE.Mesh(geo, mat);
    mesh.renderOrder = 5;
    mesh.frustumCulled = false;
    const lines = new THREE.LineSegments(new THREE.BufferGeometry(), lineMat);
    lines.renderOrder = 6;
    lines.frustumCulled = false;
    P.prim = { mesh, lines, total, hasData: false };
    P.group.add(mesh, lines);
    this.applyClip();
  }

  buildSkeleton(P) {
    const sk = P.meta.skeleton || {};
    const nv = (sk.vertices || []).length;
    const edges = sk.edges || [];
    const rV = 0.9 * P.mark;
    const rE = 0.3 * P.mark;
    const vMat = new THREE.MeshPhongMaterial({ color: this.colors.vertex, shininess: 70, specular: 0x555555 });
    const eMat = new THREE.MeshLambertMaterial({ color: this.colors.edge });
    P.themed.push([vMat, 'vertex'], [eMat, 'edge']);
    P.skV = new THREE.InstancedMesh(new THREE.SphereGeometry(rV, 20, 14), vMat, Math.max(1, nv));
    P.skE = new THREE.InstancedMesh(new THREE.CylinderGeometry(rE, rE, 1, 12, 1, false), eMat, Math.max(1, edges.length));
    P.skV.count = nv;
    P.skE.count = edges.length;
    for (const m of [P.skV, P.skE]) { m.frustumCulled = false; m.instanceMatrix.setUsage(THREE.DynamicDrawUsage); }
    P.skVGhost = xrayGhost(P.skV, this.colors.vertex, 0.7);
    P.skEGhost = xrayGhost(P.skE, this.colors.edge, 0.55);
    P.themed.push([P.skVGhost.material, 'vertex'], [P.skEGhost.material, 'edge']);
    P.edges = edges;
    P.group.add(P.skV, P.skE, P.skVGhost, P.skEGhost);
    this.setSkeleton(sk.vertices || []);
  }

  buildInitialSkeleton(P) {
    const sk = P.meta.skeleton || {};
    const verts = sk.vertices || [];
    const pos = [];
    for (const [i, j] of sk.edges || []) {
      if (verts[i] && verts[j]) pos.push(...verts[i], ...verts[j]);
    }
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.Float32BufferAttribute(pos, 3));
    const mat = new THREE.LineDashedMaterial({ color: this.colors.initial, dashSize: P.c * 1.2, gapSize: P.c * 0.8, transparent: true, opacity: 0.9, depthTest: false });
    P.themed.push([mat, 'initial']);
    const lines = new THREE.LineSegments(geo, mat);
    lines.computeLineDistances();
    lines.renderOrder = 12;
    const ptMat = new THREE.MeshBasicMaterial({ color: this.colors.initial, transparent: true, opacity: 0.85, depthTest: false });
    P.themed.push([ptMat, 'initial']);
    const pts = new THREE.InstancedMesh(new THREE.SphereGeometry(0.45 * P.mark, 12, 8), ptMat, Math.max(1, verts.length));
    pts.count = verts.length;
    pts.renderOrder = 12;
    pts.frustumCulled = false;
    const m = new THREE.Matrix4();
    verts.forEach((v, i) => { m.makeTranslation(v[0], v[1], v[2]); pts.setMatrixAt(i, m); });
    P.initial = new THREE.Group();
    P.initial.add(lines, pts);
    P.group.add(P.initial);
  }

  buildBoundaryConditions(P) {
    const bc = P.meta.boundary_conditions || {};
    const fixed = bc.fixed_vertices || [];
    const loads = bc.loads || [];
    const c = P.c;
    const s = Math.max(1, Math.max(...P.res) / 80);
    const mat = new THREE.MeshLambertMaterial({ color: this.colors.bc });
    P.themed.push([mat, 'bc']);
    P.bcs = new THREE.Group();
    const m = new THREE.Matrix4();

    if (fixed.length) {
      const cube = 0.42 * c * s;
      const mesh = new THREE.InstancedMesh(new THREE.BoxGeometry(cube, cube, cube), mat, fixed.length);
      fixed.forEach((g, i) => { m.makeTranslation(g[0] * c, g[1] * c, g[2] * c); mesh.setMatrixAt(i, m); });
      mesh.frustumCulled = false;
      const ghost = xrayGhost(mesh, this.colors.bc, 0.22);
      P.themed.push([ghost.material, 'bc']);
      P.bcs.add(mesh, ghost);
    }

    if (loads.length) {
      // every loaded grid vertex gets a small marker; each connected patch of loaded vertices gets one arrow along
      // its total force (one arrow per vertex would turn into a bundle). The arrow starts at the patch and points
      // along the force, which keeps it outside the material in the usual load cases.
      const markR = 0.3 * c * s;
      const markers = new THREE.InstancedMesh(new THREE.SphereGeometry(1, 10, 8), mat, loads.length);
      loads.forEach((l, i) => {
        m.makeScale(markR, markR, markR).setPosition(l.vertex[0] * c, l.vertex[1] * c, l.vertex[2] * c);
        markers.setMatrixAt(i, m);
      });
      markers.frustumCulled = false;
      P.bcs.add(markers);

      let patches = loadPatches(loads);
      if (patches.length > MAX_ARROWS) patches = patches.filter((_, i) => i % Math.ceil(patches.length / MAX_ARROWS) === 0);
      const mags = patches.map((p) => Math.hypot(...p.force));
      const fmax = Math.max(...mags, 1e-30);
      const L0 = THREE.MathUtils.clamp(0.15 * Math.max(...P.res), 5, 16) * c;
      const shaftGeo = new THREE.CylinderGeometry(1, 1, 1, 12).translate(0, 0.5, 0);
      const headGeo = new THREE.ConeGeometry(1, 1, 20).translate(0, 0.5, 0);
      const shafts = new THREE.InstancedMesh(shaftGeo, mat, patches.length);
      const heads = new THREE.InstancedMesh(headGeo, mat, patches.length);
      const q = new THREE.Quaternion();
      const dir = new THREE.Vector3();
      const tip = new THREE.Vector3();
      const tail = new THREE.Vector3();
      const scale = new THREE.Vector3();
      patches.forEach((p, i) => {
        dir.fromArray(p.force);
        if (mags[i] === 0) dir.set(0, -1, 0); else dir.divideScalar(mags[i]);
        // the tail sits on the side of the patch the force points to
        let reach = 0;
        for (const v of p.vertices) {
          reach = Math.max(reach, (v[0] - p.center[0]) * dir.x + (v[1] - p.center[1]) * dir.y + (v[2] - p.center[2]) * dir.z);
        }
        tail.set(p.center[0] * c, p.center[1] * c, p.center[2] * c).addScaledVector(dir, reach * c + markR);
        const L = L0 * (0.5 + 0.5 * (mags[i] / fmax));
        const head = Math.min(0.4 * L, 2.8 * c * s);
        tip.copy(tail).addScaledVector(dir, L);
        P.extraPoints.push(tip.clone());
        q.setFromUnitVectors(Y_UP, dir);
        m.compose(tail, q, scale.set(0.22 * c * s, L - head, 0.22 * c * s));
        shafts.setMatrixAt(i, m);
        m.compose(tail.addScaledVector(dir, L - head), q, scale.set(0.75 * c * s, head, 0.75 * c * s));
        heads.setMatrixAt(i, m);
      });
      for (const mesh of [shafts, heads]) {
        mesh.frustumCulled = false;
        const ghost = xrayGhost(mesh, this.colors.bc, 0.45);
        P.themed.push([ghost.material, 'bc']);
        P.bcs.add(mesh, ghost);
      }
    }
    P.group.add(P.bcs);
  }

  // ---------------------------------------------------------------------------------------- frames

  /** frame: {theta?: Uint8Array|null, positions?: Float32Array|null, vertices?: [[x,y,z]]} */
  setFrame(frame) {
    const P = this.problem;
    if (!P) return;
    if ('theta' in frame) {
      this.theta = frame.theta && frame.theta.length === P.numVoxels ? frame.theta : null;
      this.updateVoxels();
    }
    if ('positions' in frame) this.updatePrimitives(frame.positions);
    if (frame.vertices) this.setSkeleton(frame.vertices);
    this.requestRender();
  }

  setSkeleton(vertices) {
    const P = this.problem;
    if (!P || !vertices) return;
    const m = new THREE.Matrix4();
    const q = new THREE.Quaternion();
    const s = new THREE.Vector3();
    const a = new THREE.Vector3();
    const b = new THREE.Vector3();
    const mid = new THREE.Vector3();
    const n = Math.min(vertices.length, P.skV.instanceMatrix.count);
    for (let i = 0; i < n; i++) {
      m.makeTranslation(vertices[i][0], vertices[i][1], vertices[i][2]);
      P.skV.setMatrixAt(i, m);
    }
    P.skV.count = P.skVGhost.count = n;
    P.edges.forEach(([i, j], e) => {
      if (!vertices[i] || !vertices[j]) return;
      a.fromArray(vertices[i]);
      b.fromArray(vertices[j]);
      mid.addVectors(a, b).multiplyScalar(0.5);
      b.sub(a);
      const len = b.length();
      if (len > 0) q.setFromUnitVectors(Y_UP, b.divideScalar(len)); else q.identity();
      m.compose(mid, q, s.set(1, len, 1));
      P.skE.setMatrixAt(e, m);
    });
    P.skV.instanceMatrix.needsUpdate = true;
    P.skE.instanceMatrix.needsUpdate = true;
    this.requestRender();
  }

  /**
   * Smallest stored byte that is drawn. The driver stores b = round(255 (theta - theta_min) / (1 - theta_min)), so b
   * stands for theta_b = theta_min + (1 - theta_min) b / 255, and a voxel is drawn when theta_b > threshold. For the
   * default threshold 0.5 this is b >= 127 (theta_127 = 0.503, theta_126 = 0.499): the optimizer's own count of
   * theta > 0.5 (filled_voxels), up to the few voxels within half a quantization step (0.002) of 0.5.
   */
  thresholdByte() {
    const tmin = this.problem ? this.problem.thetaMin : 0.01;
    const b = Math.floor((255 * (this.threshold - tmin)) / (1 - tmin) + 1e-9) + 1;
    return THREE.MathUtils.clamp(b, 0, 256);
  }

  setThreshold(t) {
    this.threshold = t;
    this.updateVoxels();
    this.requestRender();
  }

  updateVoxels() {
    const P = this.problem;
    if (!P) return;
    const mesh = P.voxels;
    const th = this.theta;
    if (!th) {
      mesh.count = 0;
      this.info = { filled: null, drawn: 0 };
      this.onInfo?.(this.info);
      return;
    }
    const thr = this.thresholdByte();
    const [nx, ny, nz] = P.res;
    const syz = ny * nz;
    const c = P.c;
    const arr = mesh.instanceMatrix.array;
    let n = 0;
    let filled = 0;
    for (let x = 0; x < nx; x++) {
      const px = (x + 0.5) * c;
      const ix = x > 0 && x < nx - 1;
      for (let y = 0; y < ny; y++) {
        const py = (y + 0.5) * c;
        const ixy = ix && y > 0 && y < ny - 1;
        let i = (x * ny + y) * nz;
        for (let z = 0; z < nz; z++, i++) {
          if (th[i] < thr) continue;
          filled++;
          // voxels whose six neighbours are all filled cannot be seen: skip them
          if (ixy && z > 0 && z < nz - 1 && th[i - syz] >= thr && th[i + syz] >= thr
              && th[i - nz] >= thr && th[i + nz] >= thr && th[i - 1] >= thr && th[i + 1] >= thr) continue;
          const o = n * 16 + 12;
          arr[o] = px;
          arr[o + 1] = py;
          arr[o + 2] = (z + 0.5) * c;
          n++;
        }
      }
    }
    mesh.count = n;
    const attr = mesh.instanceMatrix;
    attr.clearUpdateRanges();
    if (n > 0) attr.addUpdateRange(0, n * 16);
    attr.needsUpdate = true;
    this.info = { filled, drawn: n };
    this.onInfo?.(this.info);
  }

  updatePrimitives(positions) {
    const P = this.problem;
    if (!P || !P.prim) return;
    const pr = P.prim;
    const attr = pr.mesh.geometry.getAttribute('position');
    if (!positions || positions.length !== attr.array.length) {
      pr.hasData = false;
      this.applyLayers();
      return;
    }
    attr.array.set(positions);
    attr.needsUpdate = true;
    pr.mesh.geometry.computeBoundingSphere();
    const old = pr.lines.geometry;
    pr.lines.geometry = new THREE.EdgesGeometry(pr.mesh.geometry, 25); // feature edges of the polyhedra
    old.dispose();
    pr.hasData = true;
    this.applyLayers();
  }

  // ---------------------------------------------------------------------------------------- layers, views

  setLayer(name, on) {
    this.layers[name] = !!on;
    this.applyLayers();
  }

  applyLayers() {
    const P = this.problem;
    if (!P) return;
    const L = this.layers;
    P.box.visible = L.box;
    P.voxels.visible = L.voxels;
    if (P.prim) P.prim.mesh.visible = P.prim.lines.visible = L.primitives && P.prim.hasData;
    for (const o of [P.skV, P.skE, P.skVGhost, P.skEGhost]) o.visible = L.skeleton;
    P.initial.visible = L.initial;
    P.bcs.visible = L.bcs;
    this.requestRender();
  }

  setClip(on) {
    this.clip = !!on;
    this.applyClip();
  }

  applyClip() {
    const P = this.problem;
    if (!P || !P.prim) return;
    for (const mat of [P.prim.mesh.material, P.prim.lines.material]) {
      mat.clippingPlanes = this.clip ? P.planes : null;
      mat.needsUpdate = true;
    }
    this.requestRender();
  }

  hasPrimitives() { return !!(this.problem && this.problem.prim && this.problem.prim.hasData); }

  /** Camera directions (from the target towards the camera) of the presets. */
  viewDirections(P) {
    const U = P.up.vec;
    const [a, b] = [0, 1, 2].filter((i) => i !== P.up.k);
    const thin = P.size.getComponent(a) <= P.size.getComponent(b) ? a : b; // plate normal for plate-like domains
    const wide = thin === a ? b : a;
    const front = AXES[thin].clone();
    const right = new THREE.Vector3().crossVectors(front.clone().negate(), U);
    if (right.dot(AXES[wide]) < 0) front.negate(); // keep the wide axis pointing to the right
    right.crossVectors(front.clone().negate(), U).normalize();
    return {
      front,
      side: right.clone(),
      // a tiny tilt towards the front keeps the orbit well defined and puts the front side at the bottom
      top: U.clone().addScaledVector(front, 1e-3).normalize(),
      iso: front.clone().addScaledVector(right, 0.8).addScaledVector(U, 0.65).normalize(),
    };
  }

  /** Plate-like domains are viewed along their thinnest axis (like the paper's figures), others from a corner. */
  defaultView(P) {
    const s = [P.size.x, P.size.y, P.size.z];
    const mn = Math.min(...s);
    if (mn / Math.max(...s) < 0.34) return s.indexOf(mn) === P.up.k ? 'top' : 'front';
    return 'iso';
  }

  setView(name) {
    const P = this.problem;
    if (!P || !this.controls) return;
    this.autoView = name;
    this.resize(); // the container may just have become visible
    if (name === 'default') name = this.defaultView(P);
    const dirs = this.viewDirections(P);
    const dir = dirs[name] || dirs.iso;
    const dist = this.fitDistance(P, dir);
    if (this.autoView === 'default') { // only the automatic view sizes the canvas (no jumps on presets)
      this.container.style.setProperty('--view-aspect', this.projectedAspect(P, dir).toFixed(3));
    }
    this.camera.up.copy(P.up.vec);
    this.camera.position.copy(P.center).addScaledVector(dir, dist);
    this.camera.lookAt(P.center);
    this.controls.target.copy(P.center);
    this.controls.update();
    this.requestRender();
  }

  /** Corners of the domain box and the tips of the load arrows, relative to the domain center. */
  fitPoints(P) {
    const pts = [];
    for (let i = 0; i < 8; i++) {
      pts.push(new THREE.Vector3(i & 1 ? P.size.x : 0, i & 2 ? P.size.y : 0, i & 4 ? P.size.z : 0).sub(P.center));
    }
    for (const p of P.extraPoints) pts.push(p.clone().sub(P.center));
    return pts;
  }

  /** Width / height of the domain box as seen from the given direction (used to size the view on phones). */
  projectedAspect(P, dir) {
    const F = dir.clone().negate();
    const R = new THREE.Vector3().crossVectors(F, P.up.vec);
    if (R.lengthSq() < 1e-12) R.set(1, 0, 0);
    R.normalize();
    const Uc = new THREE.Vector3().crossVectors(R, F).normalize();
    let w = 0;
    let h = 0;
    for (const q of this.fitPoints(P)) {
      w = Math.max(w, Math.abs(q.dot(R)));
      h = Math.max(h, Math.abs(q.dot(Uc)));
    }
    return THREE.MathUtils.clamp(w / Math.max(h, 1e-9), 0.6, 4);
  }

  /** Distance at which the whole domain box fits into the view for the given camera direction. */
  fitDistance(P, dir) {
    const F = dir.clone().negate();
    const R = new THREE.Vector3().crossVectors(F, P.up.vec);
    if (R.lengthSq() < 1e-12) R.set(1, 0, 0);
    R.normalize();
    const Uc = new THREE.Vector3().crossVectors(R, F).normalize();
    const tv = Math.tan(THREE.MathUtils.degToRad(this.camera.fov / 2));
    const th = tv * this.camera.aspect;
    const margin = 1.14;
    let d = 0;
    for (const q of this.fitPoints(P)) {
      const x = Math.abs(q.dot(R)) * margin;
      const y = Math.abs(q.dot(Uc)) * margin;
      const z = q.dot(F);
      d = Math.max(d, x / th - z, y / tv - z);
    }
    return Math.max(d, P.maxDim * 0.2);
  }

  // ---------------------------------------------------------------------------------------- gizmo, export

  buildGizmo() {
    if (!this.gizmo) return;
    const NS = 'http://www.w3.org/2000/svg';
    this.gizmoItems = AXES.map((_, i) => {
      const g = document.createElementNS(NS, 'g');
      const line = document.createElementNS(NS, 'line');
      line.setAttribute('x1', '0');
      line.setAttribute('y1', '0');
      line.setAttribute('stroke', AXIS_COLORS[i]);
      const text = document.createElementNS(NS, 'text');
      text.textContent = AXIS_NAMES[i];
      g.append(line, text);
      this.gizmo.append(g);
      return { g, line, text };
    });
  }

  updateGizmo() {
    if (!this.gizmoItems) return;
    const inv = this.camera.quaternion.clone().invert();
    const v = new THREE.Vector3();
    const order = AXES.map((a, i) => {
      v.copy(a).applyQuaternion(inv);
      const it = this.gizmoItems[i];
      it.line.setAttribute('x2', (v.x * 20).toFixed(1));
      it.line.setAttribute('y2', (-v.y * 20).toFixed(1));
      it.text.setAttribute('x', (v.x * 27).toFixed(1));
      it.text.setAttribute('y', (-v.y * 27).toFixed(1));
      it.g.setAttribute('opacity', v.z < -0.2 ? '0.55' : '1');
      return { i, z: v.z };
    });
    order.sort((p, q) => p.z - q.z).forEach(({ i }) => this.gizmo.append(this.gizmoItems[i].g));
  }

  screenshot(filename, done) {
    if (!this.available) return;
    this.render();
    this.renderer.domElement.toBlob((blob) => done?.(blob, filename), 'image/png');
  }
}
