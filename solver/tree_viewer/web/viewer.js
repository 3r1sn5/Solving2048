"use strict";

await (async () => {
  const $ = id => document.getElementById(id);
  const canvas = $("tree");
  const context = canvas.getContext("2d");
  const numberFormat = new Intl.NumberFormat("en");
  const compactNumberFormat = new Intl.NumberFormat("en", {
    notation: "compact",
    maximumFractionDigits: 2,
  });
  const plotPadding = 30;
  const state = {
    meta: null,
    rootTrajectory: null,
    view: { x0: 1, x1: 2, y0: 0, y1: 1 },
    initialSpan: 1,
    payload: null,
    viewportReady: false,
    points: [],
    selected: null,
    selectedPath: new Set(),
    selectionRequest: 0,
    request: 0,
    timer: null,
    canvasSize: { width: 1, height: 1, ratio: 1 },
    pointerDown: false,
    dragging: false,
    dragStart: null,
  };

  const colors = {
    player: "#147b8a", environment: "#b66b1c", terminal: "#d34949",
    rzone: "#0aa36c", tt: "#7857c5", edge: "rgba(80,99,110,.22)",
  };

  const outcomeFills = {
    WIN: "#dcf5e8", LOSS: "#ffe8dd", UNKNOWN: "#f6f7f9",
  };

  function compact(value) {
    const numeric = Number(value);
    return (numeric >= 10000 ? compactNumberFormat : numberFormat).format(numeric);
  }

  function full(value) { return numberFormat.format(Number(value)); }

  async function api(path) {
    const response = await fetch(path);
    const value = await response.json();
    if (!response.ok) throw new Error(value.error || response.statusText);
    return value;
  }

  function summaryItem(label, value, note) {
    const item = document.createElement("article");
    item.className = "summary-item";
    item.innerHTML = `<span>${label}</span><strong>${value}</strong><small>${note}</small>`;
    return item;
  }

  function renderSummary() {
    const root = state.meta.root_statistics || {};
    const values = [
      ["SGF tree nodes", full(state.meta.node_count), "visited + proof leaves"],
      ["Maximum depth", full(state.meta.maximum_depth), "search trajectory"],
      ["Winning tile", full(state.meta.winning_tile), state.meta.outcome],
      ["RZone", state.meta.has_rzone ? "available" : "not attached", root.rzone_mode || "—"],
      ["TT entries", state.meta.tt_summary ? compact(state.meta.tt_summary.player_records + state.meta.tt_summary.environment_records) : "—", state.meta.tt_summary?.symmetry ? "symmetry" : "exact"],
      ["SGF source", `${(state.meta.sgf_size / 1073741824).toFixed(state.meta.sgf_size > 1073741824 ? 2 : 3)} GiB`, "stream-indexed once"],
    ];
    $("summary").innerHTML = "";
    for (const value of values) $("summary").appendChild(summaryItem(...value));
  }

  function resize() {
    const bounds = canvas.getBoundingClientRect();
    const ratio = window.devicePixelRatio || 1;
    const width = Math.max(1, bounds.width);
    const height = Math.max(1, bounds.height);
    const pixelWidth = Math.round(width * ratio);
    const pixelHeight = Math.round(height * ratio);
    const ratioChanged = ratio !== state.canvasSize.ratio;
    const layoutChanged = width !== state.canvasSize.width ||
      height !== state.canvasSize.height || ratioChanged;
    const bitmapChanged = canvas.width !== pixelWidth || canvas.height !== pixelHeight;
    state.canvasSize = { width, height, ratio };
    if (bitmapChanged) {
      canvas.width = pixelWidth;
      canvas.height = pixelHeight;
    }
    if (bitmapChanged || ratioChanged) {
      context.setTransform(ratio, 0, 0, ratio, 0, 0);
    }
    if (layoutChanged || bitmapChanged) render();
  }

  function width() { return state.canvasSize.width; }
  function height() { return state.canvasSize.height; }
  function plotWidth() { return Math.max(1, width() - plotPadding * 2); }
  function plotHeight() { return Math.max(1, height() - plotPadding * 2); }
  function screenX(value) { return plotPadding + (value - state.view.x0) / (state.view.x1 - state.view.x0) * plotWidth(); }
  function screenY(value) { return plotPadding + (value - state.view.y0) / (state.view.y1 - state.view.y0) * plotHeight(); }
  function worldX(value) { return state.view.x0 + (value - plotPadding) / plotWidth() * (state.view.x1 - state.view.x0); }
  function worldY(value) { return state.view.y0 + (value - plotPadding) / plotHeight() * (state.view.y1 - state.view.y0); }

  function fit() {
    if (!state.meta) return;
    state.view = { x0: 1, x1: Math.max(2, state.meta.node_count), y0: 0, y1: Math.max(1, state.meta.maximum_depth + 1) };
    state.initialSpan = state.view.x1 - state.view.x0;
    scheduleLoad(0);
  }

  function frameRootTrajectory() {
    const path = state.rootTrajectory?.path || [];
    if (!path.length) return;
    const framed = path.slice(0, 14);
    const last = framed[framed.length - 1];
    const maximumId = Math.max(...framed.map(node => node.id));
    const maximumDepth = Math.max(...framed.map(node => node.depth));
    const xPadding = 6;
    const yPadding = 5;
    state.view = {
      x0: 1,
      x1: Math.min(state.meta.node_count, Math.max(20, maximumId + xPadding)),
      y0: 0,
      y1: Math.min(state.meta.maximum_depth + 1, Math.max(8, maximumDepth + yPadding)),
    };
    $("viewport-status").textContent = `Root trajectory start · #1–#${full(last.id)} shown · ${full(path.length)} nodes total`;
  }

  function showRootTrajectory() {
    if (!state.meta || !state.rootTrajectory) return;
    frameRootTrajectory();
    scheduleLoad(0);
    selectNode(1);
  }

  function clampView() {
    const xSpan = Math.max(1, state.view.x1 - state.view.x0);
    const ySpan = Math.max(1, state.view.y1 - state.view.y0);
    if (state.view.x0 < 1) { state.view.x0 = 1; state.view.x1 = 1 + xSpan; }
    if (state.view.x1 > state.meta.node_count) { state.view.x1 = state.meta.node_count; state.view.x0 = Math.max(1, state.view.x1 - xSpan); }
    if (state.view.y0 < 0) { state.view.y0 = 0; state.view.y1 = ySpan; }
    if (state.view.y1 > state.meta.maximum_depth + 1) { state.view.y1 = state.meta.maximum_depth + 1; state.view.y0 = Math.max(0, state.view.y1 - ySpan); }
  }

  function zoomAt(factor, x, y) {
    if (!state.meta) return;
    const previous = { ...state.view };
    const anchorX = worldX(x), anchorY = worldY(y);
    const xSpan = Math.max(4, (state.view.x1 - state.view.x0) * factor);
    const ySpan = Math.max(4, (state.view.y1 - state.view.y0) * factor);
    const xRatio = Math.max(0, Math.min(1, (x - plotPadding) / plotWidth()));
    const yRatio = Math.max(0, Math.min(1, (y - plotPadding) / plotHeight()));
    state.view.x0 = anchorX - xSpan * xRatio; state.view.x1 = state.view.x0 + xSpan;
    state.view.y0 = anchorY - ySpan * yRatio; state.view.y1 = state.view.y0 + ySpan;
    clampView();
    const unchanged = ["x0", "x1", "y0", "y1"].every(
      key => Math.abs(state.view[key] - previous[key]) < 1e-9);
    if (unchanged) return;
    scheduleLoad();
  }

  function scheduleLoad(delay = 80) {
    clearTimeout(state.timer);
    const sequence = ++state.request;
    state.viewportReady = false;
    $("loading").hidden = false;
    render();
    state.timer = setTimeout(() => loadViewport(sequence), delay);
  }

  async function loadViewport(sequence = ++state.request) {
    if (!state.meta) return;
    $("loading").hidden = false;
    const from = Math.max(1, Math.floor(state.view.x0));
    const to = Math.min(state.meta.node_count, Math.ceil(state.view.x1));
    const depthMin = Math.max(0, Math.floor(state.view.y0));
    const depthMax = Math.min(state.meta.maximum_depth, Math.ceil(state.view.y1));
    try {
      const payload = await api(`/api/tree?from=${from}&to=${to}&depth_min=${depthMin}&depth_max=${depthMax}&limit=9000`);
      if (sequence !== state.request) return;
      state.payload = payload;
      state.viewportReady = true;
      $("viewport-status").textContent = `${payload.mode} · ${full(payload.nodes.length)} rendered · nodes ${full(from)}–${full(to)} · depth ${depthMin}–${depthMax}`;
      render();
    } catch (error) {
      if (sequence === state.request) $("viewport-status").textContent = `Viewport error: ${error.message}`;
    } finally {
      if (sequence === state.request) $("loading").hidden = true;
    }
  }

  function pointColor(node) {
    if (node.solved_by === "TERMINAL") return colors.terminal;
    if (node.solved_by === "TT") return colors.tt;
    if (node.rz_certificate) return colors.rzone;
    return node.phase === "player" ? colors.player : colors.environment;
  }

  function renderOverview(canvasWidth, canvasHeight) {
    if (!state.payload?.overview?.length) return;
    const max = Math.max(1, ...state.payload.overview.map(item => item.total));
    for (const item of state.payload.overview) {
      const x0 = screenX(item.start), x1 = screenX(item.end);
      if (x1 < 0 || x0 > canvasWidth) continue;
      const alpha = .03 + .13 * item.total / max;
      context.fillStyle = `rgba(20,123,138,${alpha})`;
      context.fillRect(x0, 0, Math.max(1, x1 - x0), canvasHeight);
      const barWidth = Math.max(1, x1 - x0);
      const bottom = canvasHeight - 3;
      const scale = Math.min(34, canvasHeight * .08) / Math.max(1, item.total);
      let offset = 0;
      for (const [key, color] of [["terminal", colors.terminal], ["rzone", colors.rzone], ["tt", colors.tt]]) {
        const bar = item[key] * scale;
        context.fillStyle = color;
        context.fillRect(x0, bottom - offset - bar, barWidth, Math.max(.7, bar));
        offset += bar;
      }
    }
  }

  function insideViewport(x, y, canvasWidth, canvasHeight) {
    return x >= 0 && x <= canvasWidth && y >= 0 && y <= canvasHeight;
  }

  function viewportIntersection(x, y, targetX, targetY, canvasWidth, canvasHeight) {
    const dx = targetX - x;
    const dy = targetY - y;
    let scale = 1;
    if (dx < 0) scale = Math.min(scale, -x / dx);
    else if (dx > 0) scale = Math.min(scale, (canvasWidth - x) / dx);
    if (dy < 0) scale = Math.min(scale, -y / dy);
    else if (dy > 0) scale = Math.min(scale, (canvasHeight - y) / dy);
    return { x: x + dx * scale, y: y + dy * scale };
  }

  function drawIncomingEdge(node, canvasWidth, canvasHeight) {
    const x = screenX(node.id);
    const y = screenY(node.depth);
    if (!node.parent || !insideViewport(x, y, canvasWidth, canvasHeight)) return;

    const parentX = screenX(node.parent);
    const parentY = screenY(node.parent_depth);
    const parentVisible = insideViewport(parentX, parentY, canvasWidth, canvasHeight);
    const start = parentVisible
      ? { x: parentX, y: parentY }
      : viewportIntersection(x, y, parentX, parentY, canvasWidth, canvasHeight);
    const active = state.selectedPath.has(node.id) && state.selectedPath.has(node.parent);
    const color = active ? "#1672b8" : colors.edge;

    context.beginPath();
    context.moveTo(start.x, start.y);
    const middle = (start.y + y) / 2;
    context.bezierCurveTo(start.x, middle, x, middle, x, y);
    context.strokeStyle = color;
    context.lineWidth = active ? 2.4 : 1.4;
    context.stroke();

    if (!parentVisible) {
      context.beginPath();
      context.arc(start.x, start.y, active ? 3 : 2.5, 0, Math.PI * 2);
      context.fillStyle = active ? "#1672b8" : "rgba(80,99,110,.55)";
      context.fill();
    }
  }

  function render() {
    const canvasWidth = width();
    const canvasHeight = height();
    context.clearRect(0, 0, canvasWidth, canvasHeight);
    state.points = [];
    if (!state.payload) return;
    renderOverview(canvasWidth, canvasHeight);
    const exact = state.payload.mode === "exact";
    if (exact) {
      for (const node of state.payload.nodes) drawIncomingEdge(node, canvasWidth, canvasHeight);
    }
    const xSpan = Math.max(1, state.view.x1 - state.view.x0);
    const radius = exact ? Math.max(6, Math.min(14, 11 - Math.log10(Math.max(1, xSpan / 20)) * 2.5)) : 2;
    const showLabels = exact && xSpan <= 80;
    for (const node of state.payload.nodes) {
      const x = screenX(node.id), y = screenY(node.depth);
      if (x < -30 || x > canvasWidth + 30 || y < -30 || y > canvasHeight + 30) continue;
      if (state.selected?.id === node.id) {
        context.beginPath(); context.arc(x, y, radius + 4, 0, Math.PI * 2);
        context.fillStyle = "rgba(22,114,184,.14)"; context.fill();
        context.strokeStyle = "#1672b8"; context.lineWidth = 2.5; context.stroke();
      }
      context.beginPath(); context.arc(x, y, radius, 0, Math.PI * 2);
      context.fillStyle = outcomeFills[node.outcome] || outcomeFills.UNKNOWN; context.fill();
      context.strokeStyle = pointColor(node); context.lineWidth = state.selected?.id === node.id ? 3 : 2; context.stroke();
      if (showLabels) {
        const kind = node.solved_by === "TERMINAL" ? "T" : node.phase === "player" ? "OR" : "AND";
        context.fillStyle = node.phase === "player" ? "#0369a1" : "#b45309";
        context.font = `900 ${kind === "AND" ? 7 : 8}px Arial, sans-serif`;
        context.textAlign = "center"; context.textBaseline = "middle";
        context.fillText(kind, x, y + .5);
        const label = node.action_label.length > 10 ? `${node.action_label.slice(0, 9)}…` : node.action_label;
        context.font = "700 9px Arial, sans-serif";
        context.fillStyle = "#344054"; context.textBaseline = "top";
        context.fillText(label, x, y + radius + 4);
      }
      state.points.push({ x, y, radius, node });
    }
    const scale = state.initialSpan / Math.max(1, state.view.x1 - state.view.x0);
    $("zoom").textContent = `${compact(scale)}×`;
  }

  function nearestPoint(x, y) {
    if (!state.viewportReady) return null;
    let best = null, distance = Infinity;
    for (const point of state.points) {
      const dx = point.x - x, dy = point.y - y;
      const candidate = dx ** 2 + dy ** 2;
      const circleHit = candidate <= Math.max(9, point.radius + 4) ** 2;
      if (circleHit && candidate < distance) { distance = candidate; best = point; }
    }
    return best;
  }

  function selectPointAt(event) {
    const bounds = canvas.getBoundingClientRect();
    const point = nearestPoint(event.clientX - bounds.left, event.clientY - bounds.top);
    if (point) selectNode(point.node.id);
  }

  function fact(label, value, title = "") {
    const wrapper = document.createElement("div"); wrapper.className = "fact";
    const term = document.createElement("dt"); term.textContent = label;
    const description = document.createElement("dd"); description.textContent = value ?? "—"; description.title = title || description.textContent;
    wrapper.append(term, description); return wrapper;
  }

  function facts(target, rows) {
    target.innerHTML = "";
    for (const row of rows) target.appendChild(fact(...row));
  }

  function renderBoard(node) {
    const board = $("board"); board.innerHTML = "";
    const mask = Number.parseInt(node?.rzone?.display_mask || "0", 16) || 0;
    const pattern = BigInt(node?.rzone?.display_masked_board || "0");
    const tiles = node?.tiles || Array(16).fill(0);
    for (let position = 0; position < 16; ++position) {
      const inside = (mask & (1 << position)) !== 0;
      const expected = Number((pattern >> BigInt(position * 4)) & 0xfn);
      const cell = document.createElement("div");
      cell.className = `board-cell${inside ? " rzone" : ""}${inside && expected === 0 ? " empty-zone" : ""}`;
      const index = document.createElement("small"); index.textContent = position;
      const value = document.createElement("span"); value.textContent = tiles[position] || "·";
      cell.append(index, value); board.appendChild(cell);
    }
  }

  async function selectNode(nodeId, center = false, reveal = false) {
    const sequence = ++state.selectionRequest;
    try {
      const node = await api(`/api/node/${nodeId}`);
      if (sequence !== state.selectionRequest) return;
      state.selected = node;
      state.selectedPath.clear();
      if (center) centerOn(node, true);
      else if (reveal) revealNode(node);
      $("selected-label").textContent = `#${full(node.id)} · depth ${node.depth} · ${node.action_label}`;
      $("selected-status").textContent = node.outcome;
      $("selected-status").className = `badge ${node.outcome.toLowerCase()}`;
      renderBoard(node);
      facts($("node-details"), [
        ["Node ID", `#${node.id}`], ["DFS depth", node.depth], ["Phase", node.phase],
        ["Incoming action", node.action_label], ["Outcome", node.outcome], ["Solved by", node.solved_by],
        ["Parent", node.parent ? `#${node.parent}` : "root"], ["Children", node.children],
        ["Skipped actions", node.skipped], ["Subtree interval", `${node.id}–${node.subtree_end}`],
        ["Board", node.board], ["TT source", node.tt_source ? `#${node.tt_source}` : node.solved_by === "TT" ? "persistent / unavailable" : "—"],
      ]);
      $("go-parent").disabled = !node.parent;
      $("go-tt").disabled = !node.tt_source;
      $("center-node").disabled = false;
      renderRZone(node);
      loadPath(node.id);
      render();
    } catch (error) {
      if (sequence === state.selectionRequest) {
        $("selected-label").textContent = `Error: ${error.message}`;
      }
    }
  }

  async function renderRZone(node) {
    const certificate = node.rzone;
    $("rz-label").textContent = certificate ? `#${certificate.id}` : "—";
    if (!certificate) {
      $("rz-details").innerHTML = "";
      $("proof-chain").innerHTML = '<div class="empty">This node has no RZone certificate.</div>';
      $("proof-count").textContent = "0 steps";
      return;
    }
    facts($("rz-details"), [
      ["Certificate", `#${certificate.id}`], ["Kind", certificate.kind], ["Phase", certificate.phase],
      ["Mask", certificate.mask], ["Masked board", certificate.masked_board], ["Depth", certificate.depth],
      ["Transform", certificate.transform], ["Child", certificate.child ? `#${certificate.child}` : "terminal"],
    ]);
    try {
      const proof = await api(`/api/rzone/${certificate.id}/proof?transform=${node.rz_transform}`);
      if (state.selected?.id !== node.id) return;
      $("proof-chain").innerHTML = ""; $("proof-count").textContent = `${proof.length} steps`;
      for (const [index, step] of proof.entries()) {
        const row = document.createElement("div");
        row.className = "proof-row";
        row.innerHTML = `<span>${index + 1}</span><span><strong>#${step.id} · ${step.kind}</strong><small>${step.phase} · ${step.mask} · depth ${step.depth}</small></span><span>${step.child ? `→ #${step.child}` : "terminal"}</span>`;
        $("proof-chain").appendChild(row);
      }
    } catch (error) { $("proof-chain").innerHTML = `<div class="empty">${error.message}</div>`; }
  }

  async function loadPath(nodeId) {
    try {
      const response = await api(`/api/node/${nodeId}/path`);
      if (state.selected?.id !== nodeId) return;
      state.selectedPath = new Set(response.path.map(node => node.id));
      $("path").innerHTML = ""; $("path-count").textContent = `${response.path.length} nodes`;
      for (const node of response.path) {
        const button = document.createElement("button"); button.type = "button"; button.className = "path-row";
        button.innerHTML = `<span>#${node.id}</span><span><strong>${node.action_label}</strong><small>${node.phase} · depth ${node.depth}</small></span><span>${node.solved_by}</span>`;
        button.addEventListener("click", () => selectNode(node.id, true)); $("path").appendChild(button);
      }
      render();
    } catch (error) {
      $("path-count").textContent = "0 nodes";
      $("path").innerHTML = `<div class="empty">${error.message}</div>`;
    }
  }

  function centerOn(node, zoom = false) {
    let xSpan = state.view.x1 - state.view.x0, ySpan = state.view.y1 - state.view.y0;
    if (zoom) { xSpan = Math.min(xSpan, 40); ySpan = Math.min(ySpan, 20); }
    state.view.x0 = node.id - xSpan / 2; state.view.x1 = node.id + xSpan / 2;
    state.view.y0 = node.depth - ySpan / 2; state.view.y1 = node.depth + ySpan / 2;
    clampView(); scheduleLoad(0);
  }

  function revealNode(node) {
    // A sampled overview cannot display an individual selection reliably, so
    // entering it through the keyboard switches to a local detail viewport.
    if (state.payload?.mode !== "exact") {
      centerOn(node, true);
      return;
    }
    const xSpan = state.view.x1 - state.view.x0;
    const ySpan = state.view.y1 - state.view.y0;
    const xMargin = Math.min(xSpan * .16, 8);
    const yMargin = Math.min(ySpan * .16, 4);
    let dx = 0, dy = 0;
    if (node.id < state.view.x0 + xMargin) dx = node.id - (state.view.x0 + xMargin);
    else if (node.id > state.view.x1 - xMargin) dx = node.id - (state.view.x1 - xMargin);
    if (node.depth < state.view.y0 + yMargin) dy = node.depth - (state.view.y0 + yMargin);
    else if (node.depth > state.view.y1 - yMargin) dy = node.depth - (state.view.y1 - yMargin);
    if (dx === 0 && dy === 0) {
      if (!state.payload.nodes.some(candidate => candidate.id === node.id)) scheduleLoad(0);
      return;
    }
    state.view.x0 += dx; state.view.x1 += dx;
    state.view.y0 += dy; state.view.y1 += dy;
    clampView(); scheduleLoad(0);
  }

  canvas.addEventListener("pointerdown", event => {
    if (event.button !== 0) return;
    event.preventDefault();
    try { canvas.focus({ preventScroll: true }); } catch (_) { canvas.focus(); }
    state.pointerDown = true;
    state.dragging = false;
    state.dragStart = { x: event.clientX, y: event.clientY, pointerId: event.pointerId, view: { ...state.view } };
    canvas.style.cursor = "";
    try { canvas.setPointerCapture(event.pointerId); } catch (_) { /* Window cleanup remains active. */ }
  });

  function updatePointerCursor(event) {
    if (state.pointerDown) return;
    const bounds = canvas.getBoundingClientRect();
    const point = nearestPoint(event.clientX - bounds.left, event.clientY - bounds.top);
    canvas.style.cursor = point ? "pointer" : "";
  }

  canvas.addEventListener("pointermove", event => {
    updatePointerCursor(event);
  });

  function movePointer(event) {
    if (!state.pointerDown || !state.dragStart) return;
    const dx = event.clientX - state.dragStart.x, dy = event.clientY - state.dragStart.y;
    if (!state.dragging) {
      if (Math.hypot(dx, dy) <= 6) return;
      state.dragging = true;
      canvas.classList.add("dragging");
    }
    const xSpan = state.dragStart.view.x1 - state.dragStart.view.x0, ySpan = state.dragStart.view.y1 - state.dragStart.view.y0;
    state.view.x0 = state.dragStart.view.x0 - dx / plotWidth() * xSpan; state.view.x1 = state.view.x0 + xSpan;
    state.view.y0 = state.dragStart.view.y0 - dy / plotHeight() * ySpan; state.view.y1 = state.view.y0 + ySpan;
    clampView(); scheduleLoad();
  }

  function finishPointer(event = null) {
    if (!state.pointerDown && !state.dragging) return;
    const shouldSelect = !state.dragging && event?.type === "pointerup" && event.button === 0;
    const pointerId = event?.pointerId ?? state.dragStart?.pointerId;
    if (pointerId !== undefined) {
      try {
        if (canvas.hasPointerCapture(pointerId)) canvas.releasePointerCapture(pointerId);
      } catch (_) { /* The pointer may already have been released by the browser. */ }
    }
    state.pointerDown = false;
    state.dragging = false;
    state.dragStart = null;
    canvas.classList.remove("dragging");
    if (shouldSelect) selectPointAt(event);
    if (event?.clientX !== undefined) updatePointerCursor(event);
  }

  window.addEventListener("pointermove", movePointer, true);
  window.addEventListener("pointerup", finishPointer, true);
  window.addEventListener("pointercancel", finishPointer, true);
  window.addEventListener("blur", finishPointer);

  function handleWheelZoom(event) {
    event.preventDefault();
    const bounds = canvas.getBoundingClientRect();
    zoomAt(event.deltaY < 0 ? .72 : 1.38, event.clientX - bounds.left, event.clientY - bounds.top);
  }
  canvas.addEventListener("wheel", handleWheelZoom, { passive: false });
  $("root-trajectory").addEventListener("click", showRootTrajectory);
  $("fit").addEventListener("click", fit);
  $("zoom-in").addEventListener("click", () => zoomAt(.62, width() / 2, height() / 2));
  $("zoom-out").addEventListener("click", () => zoomAt(1.62, width() / 2, height() / 2));
  $("jump-form").addEventListener("submit", event => { event.preventDefault(); const value = Number.parseInt($("jump-id").value, 10); if (value) selectNode(value, true); });
  $("go-parent").addEventListener("click", () => state.selected?.parent && selectNode(state.selected.parent, true));
  $("go-tt").addEventListener("click", () => state.selected?.tt_source && selectNode(state.selected.tt_source, true));
  $("center-node").addEventListener("click", () => state.selected && centerOn(state.selected, true));

  function ignoresTreeKeyboard(event) {
    const target = event.target;
    return target?.isContentEditable || ["BUTTON", "INPUT", "SELECT", "TEXTAREA"].includes(target?.tagName);
  }

  function handleTreeKeyboard(event) {
    if (!state.meta || ignoresTreeKeyboard(event)) return;
    if (!["ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "Home"].includes(event.key)) return;
    const current = state.selected || state.rootTrajectory?.path?.[0];
    if (!current) return;
    let target = 0;
    if (event.key === "ArrowUp") {
      target = current.parent;
    } else if (event.key === "ArrowDown") {
      target = current.first_child;
    } else if (event.key === "ArrowLeft") {
      target = current.previous_sibling;
    } else if (event.key === "ArrowRight") {
      target = current.next_sibling;
    } else if (event.key === "Home") {
      target = 1;
    } else {
      return;
    }
    event.preventDefault();
    if (target) selectNode(target, false, true);
  }

  window.addEventListener("keydown", handleTreeKeyboard);
  const resizeObserver = window.ResizeObserver ? new ResizeObserver(resize) : null;
  if (resizeObserver) resizeObserver.observe($("canvas-wrap"));
  else window.addEventListener("resize", resize);

  async function start() {
    renderBoard(null);
    try {
      [state.meta, state.rootTrajectory] = await Promise.all([
        api("/api/meta"),
        api("/api/trajectory/root"),
      ]);
      state.initialSpan = Math.max(1, state.meta.node_count - 1);
      $("dataset").textContent = `${state.meta.sgf} · ${full(state.meta.node_count)} nodes`;
      renderSummary(); frameRootTrajectory(); resize();
      await loadViewport();
    } catch (error) { $("dataset").textContent = `Viewer server error: ${error.message}`; }
  }
  await start();
})();
