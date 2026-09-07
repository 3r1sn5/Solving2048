"use strict";

const BOARD_SIZE = 4;
const SLIDE_NAMES = ["up", "right", "down", "left"];

function makeNode(parent) {
  return {
    id: 0,
    parent,
    children: [],
    properties: Object.create(null),
    comment: "",
    actionPlayer: null,
    action: null,
    actionRaw: "",
    synthetic: false,
    syntheticLabel: "",
    traceNodeId: 0,
  };
}

function addProperty(node, key, value) {
  if (!node.properties[key]) node.properties[key] = [];
  node.properties[key].push(value);
  if (key === "C") node.comment = node.properties.C.join("\n");
  if ((key === "B" || key === "W") && node.actionPlayer === null) {
    node.actionPlayer = key;
    node.actionRaw = value;
    node.action = /^\d+$/.test(value) ? Number(value) : null;
  }
}

/** Parse the SGF collection while preserving sequence and variation order. */
function parseSgf(text) {
  const roots = [];
  const nodes = [];
  const variationStack = [];
  let current = null;
  let variationParent = null;
  let mode = "normal";
  let key = "";
  let value = "";
  let escaped = false;
  let skipLineFeed = false;

  function createNode(parent) {
    const node = makeNode(parent);
    node.id = nodes.length + 1;
    nodes.push(node);
    if (parent) parent.children.push(node);
    else roots.push(node);
    return node;
  }

  function consume(ch) {
    if (skipLineFeed) {
      skipLineFeed = false;
      if (ch === "\n") return;
    }
    let again = true;
    while (again) {
      again = false;
      if (mode === "normal") {
        if (ch === "(") {
          variationStack.push(variationParent);
          current = variationParent;
        } else if (ch === ")") {
          if (!variationStack.length) throw new Error("SGF 有多餘的 ')'。");
          variationParent = variationStack.pop();
          current = variationParent;
        } else if (ch === ";") {
          current = createNode(current);
          variationParent = current;
        } else if (/[A-Za-z]/.test(ch) && current) {
          key = ch.toUpperCase();
          mode = "key";
        }
      } else if (mode === "key") {
        if (/[A-Za-z]/.test(ch)) {
          key += ch.toUpperCase();
        } else if (ch === "[") {
          value = "";
          escaped = false;
          mode = "value";
        } else {
          mode = "normal";
          again = true;
        }
      } else if (mode === "after-value") {
        if (ch === "[") {
          value = "";
          escaped = false;
          mode = "value";
        } else {
          mode = "normal";
          again = true;
        }
      } else if (mode === "value") {
        if (escaped) {
          // SGF escaped line breaks are soft line breaks and contribute no text.
          if (ch === "\r") skipLineFeed = true;
          else if (ch !== "\n") value += ch;
          escaped = false;
        } else if (ch === "\\") {
          escaped = true;
        } else if (ch === "]") {
          addProperty(current, key, value);
          mode = "after-value";
        } else {
          value += ch;
        }
      }
    }
  }

  for (let i = 0; i < text.length; i++) consume(text[i]);
  if (mode === "value") throw new Error("SGF property 缺少結尾 ']'。");
  if (variationStack.length) throw new Error("SGF game tree 缺少結尾 ')'。");
  if (!roots.length) throw new Error("找不到 SGF node。");

  return { root: roots[0], roots, nodes };
}

function commentValue(node, key) {
  if (!node || !node.comment) return "";
  const escapedKey = key.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
  const match = node.comment.match(new RegExp(`^\\s*${escapedKey}\\s*(?:=|:)\\s*([^\\r\\n]*)`, "im"));
  return match ? match[1].trim() : "";
}

function statusOf(node) {
  if (node && node.synthetic) return "SKIPPED";
  const value = commentValue(node, "solver_status").toUpperCase();
  return ["WIN", "LOSS", "UNKNOWN", "SKIPPED"].includes(value) ? value : "UNKNOWN";
}

function traceNodeIdOf(node) {
  if (node && Number.isInteger(node.traceNodeId) && node.traceNodeId > 0) {
    return String(node.traceNodeId);
  }
  const value = commentValue(node, "node_id");
  return /^\d+$/.test(value) ? value : "";
}

function dfsVisitOrderOf(node) {
  if (!node || node.synthetic) return "";
  const explicit = commentValue(node, "dfs_visit_order");
  return /^\d+$/.test(explicit) ? explicit : traceNodeIdOf(node);
}

function ttSourceNodeIdOf(node) {
  const value = commentValue(node, "tt_source_node_id");
  return /^\d+$/.test(value) ? value : "";
}

function isPersistentTtHit(node) {
  return isTtHit(node) && !ttSourceNodeIdOf(node);
}

function ttReferenceLabel(node) {
  if (isPersistentTtHit(node)) return "persistent";
  const sourceId = ttSourceNodeIdOf(node);
  return `#${sourceId || "?"}`;
}

function isSymmetryTtHit(node) {
  if (!node || node.synthetic) return false;
  const value = commentValue(node, "tt_symmetry_hit").toLowerCase();
  return value === "true" || value === "1";
}

function isTtHit(node) {
  if (!node || node.synthetic) return false;
  const flag = commentValue(node, "tt_hit").toLowerCase();
  const reason = commentValue(node, "terminal_reason").toUpperCase();
  const solvedBy = commentValue(node, "solved_by").toUpperCase();
  return solvedBy === "TT" || flag === "true" || flag === "1" ||
    reason === "TRANSPOSITION_TABLE" || Boolean(ttSourceNodeIdOf(node));
}

function solvedByOf(node) {
  if (!node) return "UNKNOWN";
  if (node.synthetic) return "SKIPPED";
  const explicit = commentValue(node, "solved_by").toUpperCase();
  if (["TERMINAL", "TT", "RZONE_TT", "BACKPROPAGATION", "NODE_LIMIT"].includes(explicit)) return explicit;
  const reason = commentValue(node, "terminal_reason").toUpperCase();
  if (isTtHit(node)) return "TT";
  if (reason === "WINNING_TILE" || reason === "NO_LEGAL_MOVE") return "TERMINAL";
  if (reason === "NODE_LIMIT") return "NODE_LIMIT";
  return "BACKPROPAGATION";
}

function wasExpanded(node) {
  const explicit = commentValue(node, "expanded").toLowerCase();
  if (explicit === "true" || explicit === "1") return true;
  if (explicit === "false" || explicit === "0") return false;
  const solvedBy = solvedByOf(node);
  return solvedBy === "BACKPROPAGATION" ||
    (solvedBy === "TERMINAL" && statusOf(node) === "LOSS");
}

function terminalReasonOf(node) {
  const explicit = commentValue(node, "terminal_reason");
  if (explicit) return explicit;
  const solvedBy = solvedByOf(node);
  if (solvedBy === "TT") return "TRANSPOSITION_TABLE";
  if (solvedBy === "NODE_LIMIT") return "NODE_LIMIT";
  if (solvedBy === "TERMINAL") return statusOf(node) === "WIN" ? "WINNING_TILE" : "NO_LEGAL_MOVE";
  return "NONE";
}

function buildTraceNodeIndex(nodes) {
  const index = new Map();
  for (const node of nodes) {
    const id = traceNodeIdOf(node);
    if (id && !index.has(id)) index.set(id, node);
  }
  return index;
}

function assignImplicitTraceNodeIds(searchRoot) {
  let nextId = 1;
  const stack = [searchRoot];
  while (stack.length) {
    const node = stack.pop();
    node.traceNodeId = nextId++;
    for (let index = node.children.length - 1; index >= 0; index--) {
      stack.push(node.children[index]);
    }
  }
}

function phaseOf(node, searchRoot) {
  const explicit = commentValue(node, "node_type").toUpperCase();
  if (explicit === "PLAYER" || explicit === "ENVIRONMENT") return explicit;
  if (node === searchRoot) return "PLAYER";
  if (node && node.actionPlayer === "B") return "ENVIRONMENT";
  if (node && node.actionPlayer === "W") return "PLAYER";
  return "PLAYER";
}

function actionName(action, player) {
  if (player === "B" && Number.isInteger(action) && action >= 0 && action < 4) {
    return SLIDE_NAMES[action];
  }
  if (player === "W" && Number.isInteger(action) && action >= 4 && action < 36) {
    const local = action - 4;
    const position = local % 16;
    const tile = local < 16 ? 2 : 4;
    return `${tile}@${position}`;
  }
  return action === null ? "invalid" : String(action);
}

function incomingLabel(node, searchRoot) {
  if (node === searchRoot) return "root";
  if (!node || !node.actionPlayer) return "SGF root";
  const id = node.action === null ? node.actionRaw : node.action;
  const friendly = node.syntheticLabel || actionName(node.action, node.actionPlayer);
  return `${node.actionPlayer}[${id}] ${friendly}`;
}

function parseSkippedActions(node) {
  const source = commentValue(node, "children_skipped_actions");
  const specs = [];
  for (const token of source.trim().split(/\s+/)) {
    if (!token) continue;
    const legacy = token.match(/^([^,\s()]+)\((-?\d+)\)$/);
    const raw = legacy ? legacy[2] : token;
    const action = Number(raw);
    if (!Number.isInteger(action) || action < 0 || action >= 36) continue;
    const player = action < 4 ? "B" : "W";
    specs.push({ label: legacy ? legacy[1] : actionName(action, player), action });
  }
  return specs;
}

function materializeSkippedNodes(root, allNodes) {
  const originalNodes = [...allNodes];
  for (const node of originalNodes) {
    const existing = new Set(node.children.map(child => `${child.actionPlayer}:${child.action}`));
    for (const spec of parseSkippedActions(node)) {
      const player = spec.action < 4 ? "B" : "W";
      const key = `${player}:${spec.action}`;
      if (existing.has(key)) continue;
      const child = makeNode(node);
      child.id = allNodes.length + 1;
      child.actionPlayer = player;
      child.action = spec.action;
      child.actionRaw = String(spec.action);
      child.synthetic = true;
      child.syntheticLabel = spec.label;
      child.comment = "solver_status: SKIPPED\nterminal_reason = SHORT_CIRCUIT";
      node.children.push(child);
      allNodes.push(child);
      existing.add(key);
    }
  }
  return root;
}

function findSearchRoot(sgfRoot) {
  // Current solver SGFs put proof metadata only on the final opening node.
  let node = sgfRoot;
  while (node) {
    if (commentValue(node, "solver_status") && commentValue(node, "winning_tile")) return node;
    // Backward compatibility with SGFs that explicitly stored node_type.
    if (commentValue(node, "node_type").toUpperCase() === "PLAYER") return node;
    node = node.children.length === 1 ? node.children[0] : null;
  }

  // Legacy fixed opening: W[4] (2@0), then W[5] (2@1).
  node = sgfRoot;
  let sawFirst = false;
  while (node) {
    if (node.actionPlayer === "W" && node.action === 4) {
      sawFirst = true;
    } else if (sawFirst && node.actionPlayer === "W" && node.action === 5) {
      return node;
    }
    node = node.children.length === 1 ? node.children[0] : null;
  }

  // A comment-marked Player node is a useful fallback for hand-written SGFs.
  const stack = [sgfRoot];
  while (stack.length) {
    const current = stack.pop();
    if (commentValue(current, "node_type").toUpperCase() === "PLAYER") return current;
    for (let i = current.children.length - 1; i >= 0; i--) stack.push(current.children[i]);
  }
  return sgfRoot;
}

function pathTo(node) {
  const path = [];
  for (let current = node; current; current = current.parent) path.push(current);
  return path.reverse();
}

function relativeDepth(node, searchRoot) {
  let depth = 0;
  for (let current = node; current && current !== searchRoot; current = current.parent) depth++;
  return depth;
}

function siblingOrderOf(node, searchRoot) {
  if (!node || node.synthetic) return "skipped";
  if (node === searchRoot || !node.parent) return "0";
  const siblings = node.parent.children.filter(child => !child.synthetic);
  const index = siblings.indexOf(node);
  return index >= 0 ? String(index) : "unknown";
}

function sumIntegerFields(node, first, second) {
  const lhs = commentValue(node, first);
  const rhs = commentValue(node, second);
  if (!/^\d+$/.test(lhs) || !/^\d+$/.test(rhs)) return "";
  return (BigInt(lhs) + BigInt(rhs)).toString();
}

function emptyBoard() {
  return Array.from({ length: BOARD_SIZE }, () => Array(BOARD_SIZE).fill(0));
}

function mergeLeft(row) {
  const values = row.filter(value => value !== 0);
  const merged = [];
  for (let i = 0; i < values.length; i++) {
    if (i + 1 < values.length && values[i] === values[i + 1]) {
      merged.push(values[i] * 2);
      i++;
    } else {
      merged.push(values[i]);
    }
  }
  while (merged.length < BOARD_SIZE) merged.push(0);
  return merged;
}

function transpose(board) {
  return board[0].map((_, column) => board.map(row => row[column]));
}

function applySlide(board, action) {
  let next = null;
  if (action === 3) next = board.map(row => mergeLeft(row));
  if (action === 1) next = board.map(row => mergeLeft([...row].reverse()).reverse());
  if (action === 0) next = transpose(transpose(board).map(row => mergeLeft(row)));
  if (action === 2) {
    next = transpose(transpose(board).map(row => mergeLeft([...row].reverse()).reverse()));
  }
  if (!next) return false;
  for (let row = 0; row < BOARD_SIZE; row++) {
    for (let column = 0; column < BOARD_SIZE; column++) board[row][column] = next[row][column];
  }
  return true;
}

function applyAction(board, node) {
  if (!node.actionPlayer) return "";
  if (node.actionPlayer === "B") {
    if (!applySlide(board, node.action)) return `無法解碼 Player action B[${node.actionRaw}]`;
    return "";
  }
  if (node.actionPlayer !== "W" || !Number.isInteger(node.action) || node.action < 4 || node.action >= 36) {
    return `無法解碼 Environment action W[${node.actionRaw}]`;
  }

  const local = node.action - 4;
  const position = local % 16;
  const tile = local < 16 ? 2 : 4;
  const row = Math.floor(position / BOARD_SIZE);
  const column = position % BOARD_SIZE;
  if (board[row][column] !== 0) {
    const previous = board[row][column];
    board[row][column] = tile;
    return `W[${node.action}] 將 ${tile} 放到非空的 position ${position}（原值 ${previous}）`;
  }
  board[row][column] = tile;
  return "";
}

function boardFor(node) {
  const board = emptyBoard();
  const warnings = [];
  for (const item of pathTo(node)) {
    const warning = applyAction(board, item);
    if (warning) warnings.push(warning);
  }
  return { board, warnings };
}

function subtreeCounts(root) {
  const counts = { actual: 0, skipped: 0 };
  const stack = [root];
  while (stack.length) {
    const node = stack.pop();
    if (node.synthetic) counts.skipped++;
    else counts.actual++;
    for (let i = node.children.length - 1; i >= 0; i--) stack.push(node.children[i]);
  }
  return counts;
}

function visibleChildren(node, showSkipped) {
  return showSkipped ? node.children : node.children.filter(child => !child.synthetic);
}

function expandVisibleSubtreeBatch(stack, expandedNodes, showSkipped, batchSize = 4096) {
  let processed = 0;
  while (stack.length && processed < batchSize) {
    const node = stack.pop();
    const children = visibleChildren(node, showSkipped);
    if (children.length) expandedNodes.add(node);
    for (let index = children.length - 1; index >= 0; index--) stack.push(children[index]);
    processed++;
  }
  return { processed, done: stack.length === 0 };
}

/**
 * Lay out only the currently expanded subtree. The returned x coordinates keep
 * each parent's children in their original SGF/DFS left-to-right order.
 * This function is intentionally DOM-free so it can be smoke-tested in GJS/Node.
 */
function layoutVisibleTree(root, expandedNodes, showSkipped = true, options = {}) {
  const nodeWidth = options.nodeWidth || 132;
  const nodeHeight = options.nodeHeight || 64;
  const siblingGap = options.siblingGap || 24;
  const levelGap = options.levelGap || 124;
  const margin = options.margin || 32;
  const expanded = expandedNodes instanceof Set ? expandedNodes : new Set(expandedNodes || []);
  const entries = [];
  const edges = [];
  const byNode = new Map();
  const stack = [{ node: root, depth: 0, parent: null }];

  while (stack.length) {
    const item = stack.pop();
    const allVisibleChildren = visibleChildren(item.node, showSkipped);
    const displayedChildren = expanded.has(item.node) ? allVisibleChildren : [];
    const entry = {
      node: item.node,
      depth: item.depth,
      x: 0,
      y: margin + nodeHeight / 2 + item.depth * levelGap,
      children: [],
      hasChildren: allVisibleChildren.length > 0,
      expanded: expanded.has(item.node),
    };
    entries.push(entry);
    byNode.set(item.node, entry);
    if (item.parent) {
      item.parent.children.push(entry);
      edges.push({ parent: item.parent, child: entry });
    }
    for (let index = displayedChildren.length - 1; index >= 0; index--) {
      stack.push({ node: displayedChildren[index], depth: item.depth + 1, parent: entry });
    }
  }

  let leafIndex = 0;
  const leafSpacing = nodeWidth + siblingGap;
  for (const entry of entries) {
    if (!entry.children.length) {
      entry.x = margin + nodeWidth / 2 + leafIndex * leafSpacing;
      leafIndex++;
    }
  }
  for (let index = entries.length - 1; index >= 0; index--) {
    const entry = entries[index];
    if (entry.children.length) {
      entry.x = (entry.children[0].x + entry.children[entry.children.length - 1].x) / 2;
    }
  }

  const maxDepth = entries.reduce((maximum, entry) => Math.max(maximum, entry.depth), 0);
  const width = Math.max(nodeWidth + margin * 2, leafIndex * leafSpacing - siblingGap + margin * 2);
  const height = nodeHeight + maxDepth * levelGap + margin * 2;
  return { nodes: entries, edges, byNode, width, height, nodeWidth, nodeHeight };
}

function prepareSgf(text) {
  const parsed = parseSgf(text);
  const searchRoot = findSearchRoot(parsed.root);
  assignImplicitTraceNodeIds(searchRoot);
  materializeSkippedNodes(parsed.root, parsed.nodes);
  const nodeByTraceId = buildTraceNodeIndex(parsed.nodes);
  return { ...parsed, searchRoot, nodeByTraceId };
}

function bootViewer() {
  const elements = {
    file: document.getElementById("sgf-file"),
    fileStatus: document.getElementById("file-status"),
    showSkipped: document.getElementById("show-skipped"),
    treeSummary: document.getElementById("tree-summary"),
    tree: document.getElementById("tree"),
    treeWorkStatus: document.getElementById("tree-work-status"),
    treeWorkMessage: document.getElementById("tree-work-message"),
    selectedLabel: document.getElementById("selected-label"),
    selectedStatus: document.getElementById("selected-status"),
    ttReference: document.getElementById("tt-reference"),
    board: document.getElementById("board"),
    boardWarning: document.getElementById("board-warning"),
    path: document.getElementById("path"),
    metrics: document.getElementById("node-metrics"),
    children: document.getElementById("children"),
    comment: document.getElementById("comment"),
    toggleCurrent: document.getElementById("toggle-current"),
    goRoot: document.getElementById("go-root"),
    goParent: document.getElementById("go-parent"),
    goChild: document.getElementById("go-child"),
    goPrev: document.getElementById("go-prev"),
    goNext: document.getElementById("go-next"),
    expandAll: document.getElementById("expand-all"),
    zoomOut: document.getElementById("zoom-out"),
    zoomIn: document.getElementById("zoom-in"),
    zoomLabel: document.getElementById("zoom-label"),
    fitGraph: document.getElementById("fit-graph"),
    resetView: document.getElementById("reset-view"),
  };

  let state = null;
  let selected = null;
  const expanded = new Set();
  let graphLayout = null;
  let graphSvg = null;
  let graphLayer = null;
  let graphZoom = 1;
  let graphPanX = 0;
  let graphPanY = 0;
  let graphNodeElements = new Map();
  let graphEdgeElements = new Map();
  let panGesture = null;
  let treeWorkInProgress = false;

  function showSkipped() {
    return elements.showSkipped.checked;
  }

  function ttSourceNode(node) {
    if (!state) return null;
    const sourceId = ttSourceNodeIdOf(node);
    return sourceId ? state.nodeByTraceId.get(sourceId) || null : null;
  }

  function statusClass(status) {
    return status.toLowerCase();
  }

  function phaseClass(phase) {
    return phase.toLowerCase();
  }

  function setButtonDisabled(button, disabled) {
    button.disabled = Boolean(disabled);
  }

  function siblingsOf(node) {
    if (!node || !node.parent) return [];
    return visibleChildren(node.parent, showSkipped());
  }

  function siblingAtOffset(node, offset) {
    const siblings = siblingsOf(node);
    const index = siblings.indexOf(node);
    if (index < 0) return null;
    return siblings[index + offset] || null;
  }

  function isWithinSearchTree(node) {
    for (let current = node; current; current = current.parent) {
      if (current === state.searchRoot) return true;
    }
    return false;
  }

  function isDescendantOf(node, ancestor) {
    for (let current = node && node.parent; current; current = current.parent) {
      if (current === ancestor) return true;
    }
    return false;
  }

  function expandPath(node) {
    let changed = false;
    for (let current = node && node.parent; current && isWithinSearchTree(current); current = current.parent) {
      if (!expanded.has(current)) {
        expanded.add(current);
        changed = true;
      }
      if (current === state.searchRoot) break;
    }
    return changed;
  }

  function setGraphPathActive(node, active) {
    for (let current = node; current && current !== state.searchRoot; current = current.parent) {
      const edge = graphEdgeElements.get(current);
      if (edge) edge.classList.toggle("active", active);
    }
  }

  function updateGraphSelection(previous, next) {
    const previousElement = graphNodeElements.get(previous);
    if (previousElement) {
      previousElement.classList.remove("selected");
      previousElement.setAttribute("aria-current", "false");
    }
    setGraphPathActive(previous, false);
    const nextElement = graphNodeElements.get(next);
    if (nextElement) {
      nextElement.classList.add("selected");
      nextElement.setAttribute("aria-current", "true");
    }
    setGraphPathActive(next, true);
  }

  function selectNode(node) {
    if (!state || !node || !isWithinSearchTree(node)) return;
    if (node.synthetic && !showSkipped()) node = node.parent;
    const previous = selected;
    selected = node;
    const expansionChanged = expandPath(node);
    const alreadyRendered = !expansionChanged && graphLayout && graphLayout.byNode.has(node) && graphNodeElements.has(node);
    if (alreadyRendered) {
      updateGraphSelection(previous, node);
      renderInspector();
      renderNavigation();
    } else {
      renderAll();
    }
    requestAnimationFrame(() => revealGraphNode(selected));
  }

  function makeSvgElement(name, attributes = {}) {
    const element = document.createElementNS("http://www.w3.org/2000/svg", name);
    for (const [key, value] of Object.entries(attributes)) element.setAttribute(key, value);
    return element;
  }

  function clampGraphZoom(value) {
    return Math.max(1e-9, Math.min(4, value));
  }

  function graphZoomLabel(value) {
    const percent = value * 100;
    if (percent >= 10) return `${Math.round(percent)}%`;
    if (percent >= 1) return `${percent.toFixed(1)}%`;
    if (percent >= 0.01) return `${percent.toFixed(2)}%`;
    return `${percent.toExponential(1)}%`;
  }

  function updateGraphTransform() {
    if (graphLayer) graphLayer.setAttribute("transform", `translate(${graphPanX} ${graphPanY}) scale(${graphZoom})`);
    elements.zoomLabel.textContent = graphZoomLabel(graphZoom);
  }

  function zoomGraphAt(factor, x, y) {
    const previousZoom = graphZoom;
    const nextZoom = clampGraphZoom(previousZoom * factor);
    if (nextZoom === previousZoom) return;
    const worldX = (x - graphPanX) / previousZoom;
    const worldY = (y - graphPanY) / previousZoom;
    graphZoom = nextZoom;
    graphPanX = x - worldX * nextZoom;
    graphPanY = y - worldY * nextZoom;
    updateGraphTransform();
  }

  function zoomGraphAtCenter(factor) {
    zoomGraphAt(factor, elements.tree.clientWidth / 2, elements.tree.clientHeight / 2);
  }

  function fitGraph() {
    if (!graphLayout) return;
    const viewportWidth = Math.max(1, elements.tree.clientWidth);
    const viewportHeight = Math.max(1, elements.tree.clientHeight);
    graphZoom = clampGraphZoom(Math.min(
      (viewportWidth - 28) / graphLayout.width,
      (viewportHeight - 28) / graphLayout.height,
      1.5,
    ));
    graphPanX = (viewportWidth - graphLayout.width * graphZoom) / 2;
    graphPanY = (viewportHeight - graphLayout.height * graphZoom) / 2;
    updateGraphTransform();
  }

  function resetGraphView() {
    if (!graphLayout || !state) return;
    graphZoom = 1;
    const rootEntry = graphLayout.byNode.get(state.searchRoot);
    graphPanX = elements.tree.clientWidth / 2 - rootEntry.x;
    graphPanY = 24 - (rootEntry.y - graphLayout.nodeHeight / 2);
    updateGraphTransform();
  }

  function revealGraphNode(node) {
    if (!graphLayout || !node) return;
    const entry = graphLayout.byNode.get(node);
    if (!entry) return;
    const padding = 34;
    const halfWidth = graphLayout.nodeWidth * graphZoom / 2;
    const halfHeight = graphLayout.nodeHeight * graphZoom / 2;
    const screenX = graphPanX + entry.x * graphZoom;
    const screenY = graphPanY + entry.y * graphZoom;
    const viewportWidth = elements.tree.clientWidth;
    const viewportHeight = elements.tree.clientHeight;

    if (screenX - halfWidth < padding) graphPanX += padding - (screenX - halfWidth);
    else if (screenX + halfWidth > viewportWidth - padding) {
      graphPanX -= screenX + halfWidth - (viewportWidth - padding);
    }
    if (screenY - halfHeight < padding) graphPanY += padding - (screenY - halfHeight);
    else if (screenY + halfHeight > viewportHeight - padding) {
      graphPanY -= screenY + halfHeight - (viewportHeight - padding);
    }
    updateGraphTransform();
  }

  function truncatedLabel(value, maximum = 17) {
    return value.length > maximum ? `${value.slice(0, maximum - 1)}…` : value;
  }

  function attachGraphInteractions(svg) {
    svg.addEventListener("pointerdown", event => {
      const targetNode = event.target.closest && event.target.closest(".graph-node");
      if (event.button !== 0 || targetNode) return;
      panGesture = {
        pointerId: event.pointerId,
        clientX: event.clientX,
        clientY: event.clientY,
        panX: graphPanX,
        panY: graphPanY,
      };
      svg.setPointerCapture(event.pointerId);
      svg.classList.add("panning");
    });
    svg.addEventListener("pointermove", event => {
      if (!panGesture || event.pointerId !== panGesture.pointerId) return;
      graphPanX = panGesture.panX + event.clientX - panGesture.clientX;
      graphPanY = panGesture.panY + event.clientY - panGesture.clientY;
      updateGraphTransform();
    });
    const finishPan = event => {
      if (!panGesture || event.pointerId !== panGesture.pointerId) return;
      panGesture = null;
      svg.classList.remove("panning");
    };
    svg.addEventListener("pointerup", finishPan);
    svg.addEventListener("pointercancel", finishPan);
    svg.addEventListener("lostpointercapture", () => {
      panGesture = null;
      svg.classList.remove("panning");
    });
    svg.addEventListener("wheel", event => {
      event.preventDefault();
      const rect = svg.getBoundingClientRect();
      const factor = event.deltaY < 0 ? 1.13 : 1 / 1.13;
      zoomGraphAt(factor, event.clientX - rect.left, event.clientY - rect.top);
    }, { passive: false });
  }

  function renderTree(renderBatchSize = Infinity) {
    const previousEntry = graphLayout && selected ? graphLayout.byNode.get(selected) : null;
    const selectedScreen = previousEntry ? {
      x: graphPanX + previousEntry.x * graphZoom,
      y: graphPanY + previousEntry.y * graphZoom,
    } : null;

    elements.tree.innerHTML = "";
    graphSvg = null;
    graphLayer = null;
    graphNodeElements = new Map();
    graphEdgeElements = new Map();
    if (!state) {
      graphLayout = null;
      const empty = document.createElement("div");
      empty.className = "empty-state";
      empty.textContent = "請先載入 SGF";
      elements.tree.appendChild(empty);
      return null;
    }

    graphLayout = layoutVisibleTree(state.searchRoot, expanded, showSkipped());
    const currentEntry = selected ? graphLayout.byNode.get(selected) : null;
    if (selectedScreen && currentEntry) {
      graphPanX = selectedScreen.x - currentEntry.x * graphZoom;
      graphPanY = selectedScreen.y - currentEntry.y * graphZoom;
    }

    const selectedPath = new Set(pathTo(selected));
    const svg = makeSvgElement("svg", {
      class: "graph-svg",
      role: "img",
      "aria-label": `2048 DFS tree graph with ${graphLayout.nodes.length} visible nodes`,
    });
    graphSvg = svg;
    const layer = makeSvgElement("g", { class: "graph-layer" });
    graphLayer = layer;

    const edgeLayer = makeSvgElement("g", { class: "graph-edges" });
    function appendEdge(edge) {
      const parent = edge.parent;
      const child = edge.child;
      const startY = parent.y + graphLayout.nodeHeight / 2;
      const endY = child.y - graphLayout.nodeHeight / 2;
      const controlY = (startY + endY) / 2;
      const active = selectedPath.has(parent.node) && selectedPath.has(child.node);
      const pathElement = makeSvgElement("path", {
        class: `graph-edge${active ? " active" : ""}`,
        d: `M ${parent.x} ${startY} C ${parent.x} ${controlY}, ${child.x} ${controlY}, ${child.x} ${endY}`,
      });
      edgeLayer.appendChild(pathElement);
      graphEdgeElements.set(child.node, pathElement);
    }
    layer.appendChild(edgeLayer);

    const nodeLayer = makeSvgElement("g", { class: "graph-nodes" });
    function appendNode(entry) {
      const node = entry.node;
      const status = statusOf(node);
      const phase = phaseOf(node, state.searchRoot);
      const ttHit = isTtHit(node);
      const symmetryTtHit = isSymmetryTtHit(node);
      const traceId = traceNodeIdOf(node);
      const visitOrder = dfsVisitOrderOf(node);
      const visitLabel = node.synthetic ? "SKIPPED" : `DFS #${visitOrder || "?"}`;
      const fullLabel = incomingLabel(node, state.searchRoot);
      const group = makeSvgElement("g", {
        class: `graph-node ${statusClass(status)}${ttHit ? " tt-hit" : ""}${node === selected ? " selected" : ""}`,
        transform: `translate(${entry.x} ${entry.y})`,
        tabindex: "0",
        role: "button",
        "data-node-id": String(node.id),
        "data-trace-node-id": traceId,
        "data-dfs-visit-order": visitOrder,
        "aria-current": node === selected ? "true" : "false",
        "aria-label": `${visitLabel}, ${fullLabel}, ${phase}, ${status}${ttHit ? `, ${symmetryTtHit ? "symmetry " : ""}TT hit referencing ${ttReferenceLabel(node)}` : ""}`,
      });
      group.appendChild(makeSvgElement("rect", {
        class: "node-body",
        x: -graphLayout.nodeWidth / 2,
        y: -graphLayout.nodeHeight / 2,
        width: graphLayout.nodeWidth,
        height: graphLayout.nodeHeight,
        rx: 9,
      }));
      if (ttHit) {
        group.appendChild(makeSvgElement("rect", {
          class: "tt-ring",
          x: -graphLayout.nodeWidth / 2 + 4,
          y: -graphLayout.nodeHeight / 2 + 4,
          width: graphLayout.nodeWidth - 8,
          height: graphLayout.nodeHeight - 8,
          rx: 6,
        }));
      }

      const orderText = makeSvgElement("text", { class: "node-order", x: 0, y: -15, "text-anchor": "middle" });
      orderText.textContent = visitLabel;
      group.appendChild(orderText);
      const titleText = makeSvgElement("text", { class: "node-title", x: 0, y: 1, "text-anchor": "middle" });
      titleText.textContent = truncatedLabel(fullLabel);
      group.appendChild(titleText);
      const metaText = makeSvgElement("text", { class: "node-meta", x: 0, y: 18, "text-anchor": "middle" });
      metaText.textContent = `${phase === "PLAYER" ? "OR" : "AND"} · ${status}${ttHit ? ` · ${symmetryTtHit ? "SYM " : ""}TT→${ttReferenceLabel(node)}` : ""}`;
      group.appendChild(metaText);

      const tooltip = makeSvgElement("title");
      const siblingOrder = siblingOrderOf(node, state.searchRoot);
      tooltip.textContent = `${fullLabel}${traceId ? `\nimplicit node id: ${traceId}` : ""}\nglobal DFS visit: ${node.synthetic ? "skipped" : visitOrder || "unknown"}\nsibling DFS order: ${siblingOrder}\n${phase} / ${phase === "PLAYER" ? "OR" : "AND"}\nstatus: ${status}\nsolved by: ${solvedByOf(node)}\ndepth: ${relativeDepth(node, state.searchRoot)}${ttHit ? `\n${symmetryTtHit ? "SYMMETRY " : ""}TT HIT -> ${ttReferenceLabel(node)}` : ""}`;
      group.appendChild(tooltip);

      if (entry.hasChildren) {
        const toggle = makeSvgElement("g", {
          class: "graph-toggle",
          transform: `translate(${graphLayout.nodeWidth / 2 - 9} ${-graphLayout.nodeHeight / 2 + 9})`,
          role: "button",
          "aria-label": entry.expanded ? "Collapse children" : "Expand children",
        });
        toggle.appendChild(makeSvgElement("circle", { r: 9 }));
        const symbol = makeSvgElement("text", { x: 0, y: 4, "text-anchor": "middle" });
        symbol.textContent = entry.expanded ? "−" : "+";
        toggle.appendChild(symbol);
        toggle.addEventListener("click", event => {
          event.stopPropagation();
          if (expanded.has(node)) {
            expanded.delete(node);
            if (selected !== node && isDescendantOf(selected, node)) selected = node;
          } else {
            expanded.add(node);
          }
          renderAll();
        });
        group.appendChild(toggle);
      }

      group.addEventListener("click", () => selectNode(node));
      group.addEventListener("keydown", event => {
        if (event.key === "Enter" || event.key === " ") {
          event.preventDefault();
          selectNode(node);
        }
      });
      nodeLayer.appendChild(group);
      graphNodeElements.set(node, group);
    }
    layer.appendChild(nodeLayer);
    svg.appendChild(layer);
    elements.tree.appendChild(svg);
    attachGraphInteractions(svg);
    updateGraphTransform();

    let edgeIndex = 0;
    let nodeIndex = 0;
    const renderJob = {
      total: graphLayout.edges.length + graphLayout.nodes.length,
      get rendered() { return edgeIndex + nodeIndex; },
      get done() {
        return edgeIndex >= graphLayout.edges.length && nodeIndex >= graphLayout.nodes.length;
      },
      step(limit = 4096) {
        let remaining = limit;
        while (edgeIndex < graphLayout.edges.length && remaining > 0) {
          appendEdge(graphLayout.edges[edgeIndex++]);
          remaining--;
        }
        while (nodeIndex < graphLayout.nodes.length && remaining > 0) {
          appendNode(graphLayout.nodes[nodeIndex++]);
          remaining--;
        }
        return this.done;
      },
    };
    renderJob.step(renderBatchSize);

    const counts = state.searchCounts || subtreeCounts(state.searchRoot);
    elements.treeSummary.textContent = `${counts.actual.toLocaleString()} search nodes · ${counts.skipped.toLocaleString()} skipped · ${graphLayout.nodes.length.toLocaleString()} visible`;
    return renderJob;
  }

  function renderBoard() {
    elements.board.innerHTML = "";
    const result = boardFor(selected);
    for (const row of result.board) {
      for (const value of row) {
        const tile = document.createElement("div");
        tile.className = "tile";
        if (value) tile.classList.add(value <= 64 ? `tile-${value}` : "tile-high");
        tile.textContent = value || "";
        elements.board.appendChild(tile);
      }
    }
    elements.boardWarning.hidden = result.warnings.length === 0;
    elements.boardWarning.textContent = result.warnings.join("；");
  }

  function renderPath() {
    elements.path.innerHTML = "";
    const path = pathTo(selected);
    const start = path.indexOf(state.searchRoot);
    const searchPath = start >= 0 ? path.slice(start) : [selected];
    searchPath.forEach((node, index) => {
      if (index) {
        const separator = document.createElement("span");
        separator.className = "path-separator";
        separator.textContent = ">";
        elements.path.appendChild(separator);
      }
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = incomingLabel(node, state.searchRoot);
      button.addEventListener("click", () => selectNode(node));
      elements.path.appendChild(button);
    });
  }

  function metric(label, value, title = "") {
    const wrapper = document.createElement("div");
    wrapper.className = "metric";
    const term = document.createElement("dt");
    term.textContent = label;
    const description = document.createElement("dd");
    description.textContent = value || "—";
    description.title = title || description.textContent;
    wrapper.append(term, description);
    return wrapper;
  }

  function renderMetrics() {
    elements.metrics.innerHTML = "";
    const phase = phaseOf(selected, state.searchRoot);
    const status = statusOf(selected);
    const nonSyntheticChildren = selected.children.filter(child => !child.synthetic).length;
    const expandedChildren = selected.children.filter(child =>
      !child.synthetic && wasExpanded(child)).length;
    const syntheticChildren = selected.children.filter(child => child.synthetic).length;
    const ttHit = isTtHit(selected);
    const symmetryTtHit = isSymmetryTtHit(selected);
    const globalVisit = dfsVisitOrderOf(selected);
    const values = [
      ["Incoming action", incomingLabel(selected, state.searchRoot)],
      ["Implicit node ID", traceNodeIdOf(selected)],
      ["Global DFS visit", selected.synthetic ? "SKIPPED" : globalVisit ? `#${globalVisit}` : "unknown"],
      ["Sibling DFS order", siblingOrderOf(selected, state.searchRoot)],
      ["DFS depth", String(relativeDepth(selected, state.searchRoot))],
      ["Node type", `${phase} / ${phase === "PLAYER" ? "OR" : "AND"}`],
      ["Result", status],
      ["Solved by", solvedByOf(selected)],
      ["TT reference", ttHit ? `HIT → ${ttReferenceLabel(selected)}` : "no"],
      ["TT symmetry hit", ttHit && commentValue(selected, "tt_symmetry_hit")
        ? (symmetryTtHit ? "yes" : "no") : "—"],
      ["RZone certificate", commentValue(selected, "rzone_certificate_id")],
      ["RZone symmetry hit", commentValue(selected, "rzone_certificate_id")
        ? (Number.parseInt(commentValue(selected, "rzone_transform") || "0", 10) !== 0
          ? "yes" : "no") : "—"],
      ["RZone transform", commentValue(selected, "rzone_transform")],
      ["Children total", String(nonSyntheticChildren + syntheticChildren)],
      ["Expanded", String(expandedChildren)],
      ["Skipped", String(syntheticChildren)],
      ["Terminal", terminalReasonOf(selected)],
    ];
    if (selected === state.searchRoot) {
      values.push(
        ["TT mode", commentValue(selected, "transposition_table")],
        ["TT symmetry mode", commentValue(selected, "tt_symmetry")],
        ["Player TT hits", commentValue(selected, "player_tt_hits")],
        ["Environment TT hits", commentValue(selected, "environment_tt_hits")],
        ["Player symmetry TT hits", commentValue(selected, "player_tt_symmetry_hits")],
        ["Environment symmetry TT hits", commentValue(selected, "environment_tt_symmetry_hits")],
        ["Player persistent TT hits", commentValue(selected, "player_persistent_tt_hits")],
        ["Environment persistent TT hits", commentValue(selected, "environment_persistent_tt_hits")],
        ["Player TT loaded", commentValue(selected, "player_tt_loaded_entries")],
        ["Environment TT loaded", commentValue(selected, "environment_tt_loaded_entries")],
        ["Player TT new", commentValue(selected, "player_tt_new_entries")],
        ["Environment TT new", commentValue(selected, "environment_tt_new_entries")],
        ["Player TT entries", sumIntegerFields(
          selected, "player_tt_loaded_entries", "player_tt_new_entries") ||
          commentValue(selected, "player_tt_entries")],
        ["Environment TT entries", sumIntegerFields(
          selected, "environment_tt_loaded_entries", "environment_tt_new_entries") ||
          commentValue(selected, "environment_tt_entries")],
        ["RZone symmetry mode", commentValue(selected, "rzone_symmetry")],
        ["RZone exact hits", commentValue(selected, "rzone_exact_orientation_hits")],
        ["RZone symmetry hits", commentValue(selected, "rzone_symmetry_hits")],
        ["RZone transforms tested", commentValue(selected, "rzone_lookup_transforms_tested")],
      );
    }
    for (const [label, value] of values) elements.metrics.appendChild(metric(label, value));
  }

  function renderChildren() {
    elements.children.innerHTML = "";
    const children = visibleChildren(selected, showSkipped());
    if (!children.length) {
      const empty = document.createElement("span");
      empty.className = "muted";
      empty.textContent = "沒有可顯示的 children";
      elements.children.appendChild(empty);
      return;
    }
    for (const child of children) {
      const ttHit = isTtHit(child);
      const button = document.createElement("button");
      button.type = "button";
      button.className = `child-row${ttHit ? " tt-hit" : ""}`;

      const action = document.createElement("span");
      action.className = "child-action";
      action.textContent = incomingLabel(child, state.searchRoot);

      const value = document.createElement("span");
      value.className = "child-value";
      value.textContent = child.synthetic
        ? "not searched"
        : ttHit
          ? `TT → ${ttReferenceLabel(child)}`
          : solvedByOf(child);

      const phase = phaseOf(child, state.searchRoot);
      const phaseBadge = document.createElement("span");
      phaseBadge.className = `phase-badge ${phaseClass(phase)}`;
      phaseBadge.textContent = phase === "PLAYER" ? "OR" : "AND";

      const status = statusOf(child);
      const statusBadge = document.createElement("span");
      statusBadge.className = `status-badge ${statusClass(status)}`;
      statusBadge.textContent = status;

      button.append(action, value, phaseBadge, statusBadge);
      button.addEventListener("click", () => selectNode(child));
      elements.children.appendChild(button);
    }
  }

  function renderInspector() {
    if (!selected) return;
    const status = statusOf(selected);
    const traceId = traceNodeIdOf(selected);
    const ttHit = isTtHit(selected);
    const symmetryTtHit = isSymmetryTtHit(selected);
    const persistentTtHit = isPersistentTtHit(selected);
    const sourceId = ttSourceNodeIdOf(selected);
    const sourceNode = ttSourceNode(selected);
    const globalVisit = dfsVisitOrderOf(selected);
    const visitLabel = selected.synthetic ? "SKIPPED" : `global DFS #${globalVisit || "?"}`;
    elements.selectedLabel.textContent = `${incomingLabel(selected, state.searchRoot)}${traceId ? ` · node #${traceId}` : ""} · ${visitLabel} · depth ${relativeDepth(selected, state.searchRoot)}`;
    elements.selectedStatus.textContent = status;
    elements.selectedStatus.className = `status-badge ${statusClass(status)}`;
    elements.ttReference.hidden = !ttHit;
    elements.ttReference.textContent = `${symmetryTtHit ? "SYMMETRY " : ""}TT HIT → ${ttReferenceLabel(selected)}`;
    elements.ttReference.className = `tt-reference${sourceNode ? "" : " unresolved"}`;
    elements.ttReference.setAttribute("aria-disabled", String(!sourceNode));
    elements.ttReference.title = persistentTtHit
      ? "Persistent transposition table entry"
      : sourceNode
      ? `跳到 ${symmetryTtHit ? "symmetry " : ""}transposition table source node #${sourceId}`
      : `TT source node #${sourceId || "?"} 不在此 SGF tree 中`;
    elements.comment.textContent = selected.comment || "(no comment)";
    elements.toggleCurrent.textContent = expanded.has(selected) ? "收合目前節點" : "展開目前節點";
    elements.toggleCurrent.disabled = visibleChildren(selected, showSkipped()).length === 0;
    renderBoard();
    renderPath();
    renderMetrics();
    renderChildren();
  }

  function nextAnimationFrame() {
    return new Promise(resolve => requestAnimationFrame(resolve));
  }

  function setTreeWorkStatus(message, visible = true) {
    elements.treeWorkMessage.textContent = message;
    elements.treeWorkStatus.hidden = !visible;
  }

  async function expandAllVisibleNodes() {
    if (!state || treeWorkInProgress) return;
    const includeSkipped = showSkipped();
    const total = state.searchCounts
      ? state.searchCounts.actual + (includeSkipped ? state.searchCounts.skipped : 0)
      : 0;
    if (total > 50_000 && typeof window !== "undefined") {
      const skippedHint = includeSkipped
        ? "\n\n如果只想觀察實際 DFS trace，可先取消「顯示 Skipped」以降低節點數。"
        : "";
      const confirmed = window.confirm(
        `展開全部將繪製 ${total.toLocaleString()} 個 nodes，可能使用數 GB 記憶體並需要較長時間。${skippedHint}\n\n是否繼續？`,
      );
      if (!confirmed) return;
    }

    treeWorkInProgress = true;
    elements.file.disabled = true;
    elements.showSkipped.disabled = true;
    elements.tree.classList.add("busy");
    renderNavigation();

    const stack = [state.searchRoot];
    let processed = 0;

    try {
      setTreeWorkStatus(`展開 tree… 0 / ${total.toLocaleString()}`);
      await nextAnimationFrame();
      while (stack.length) {
        const batch = expandVisibleSubtreeBatch(stack, expanded, includeSkipped);
        processed += batch.processed;
        setTreeWorkStatus(`展開 tree… ${processed.toLocaleString()} / ${total.toLocaleString()}`);
        if (!batch.done) await nextAnimationFrame();
      }

      setTreeWorkStatus(`計算 ${processed.toLocaleString()} 個 visible nodes 的 layout…`);
      await nextAnimationFrame();
      const renderJob = renderTree(0);
      renderInspector();
      renderNavigation();
      while (renderJob && !renderJob.done) {
        renderJob.step();
        setTreeWorkStatus(`繪製 graph… ${renderJob.rendered.toLocaleString()} / ${renderJob.total.toLocaleString()}`);
        if (!renderJob.done) await nextAnimationFrame();
      }
      setTreeWorkStatus(`Fit ${graphLayout.nodes.length.toLocaleString()} 個 nodes…`);
      await nextAnimationFrame();
      fitGraph();
      setTreeWorkStatus(`已展開 ${graphLayout.nodes.length.toLocaleString()} 個 nodes`);
      await new Promise(resolve => setTimeout(resolve, 650));
    } catch (error) {
      setTreeWorkStatus(`展開失敗：${error.message}`);
      await new Promise(resolve => setTimeout(resolve, 1600));
    } finally {
      treeWorkInProgress = false;
      elements.file.disabled = false;
      elements.showSkipped.disabled = false;
      elements.tree.classList.remove("busy");
      setTreeWorkStatus("", false);
      renderNavigation();
    }
  }

  function renderNavigation() {
    const graphButtons = [elements.zoomOut, elements.zoomIn, elements.fitGraph, elements.resetView];
    for (const button of graphButtons) setButtonDisabled(button, !selected || treeWorkInProgress);
    setButtonDisabled(elements.expandAll, !selected || treeWorkInProgress);
    if (!selected) {
      for (const button of [elements.goRoot, elements.goParent, elements.goChild, elements.goPrev, elements.goNext]) {
        setButtonDisabled(button, true);
      }
      return;
    }
    const parent = selected === state.searchRoot ? null : selected.parent;
    setButtonDisabled(elements.goRoot, treeWorkInProgress || selected === state.searchRoot);
    setButtonDisabled(elements.goParent, treeWorkInProgress || !parent);
    setButtonDisabled(elements.goChild, treeWorkInProgress || visibleChildren(selected, showSkipped()).length === 0);
    setButtonDisabled(elements.goPrev, treeWorkInProgress || !siblingAtOffset(selected, -1));
    setButtonDisabled(elements.goNext, treeWorkInProgress || !siblingAtOffset(selected, 1));
  }

  function renderAll() {
    renderTree();
    renderInspector();
    renderNavigation();
  }

  function formatBytes(bytes) {
    const units = ["B", "KB", "MB", "GB"];
    let value = bytes;
    let unit = 0;
    while (value >= 1024 && unit + 1 < units.length) {
      value /= 1024;
      unit++;
    }
    return `${value.toFixed(unit === 0 ? 0 : 1)} ${units[unit]}`;
  }

  async function loadFile(file) {
    elements.file.disabled = true;
    elements.fileStatus.textContent = `載入 ${file.name}…`;
    try {
      // This first-version viewer intentionally loads the full SGF in memory.
      const text = await file.text();
      state = prepareSgf(text);
      selected = state.searchRoot;
      expanded.clear();
      expanded.add(state.searchRoot);
      graphLayout = null;
      graphZoom = 1;
      graphPanX = 0;
      graphPanY = 0;
      const counts = subtreeCounts(state.searchRoot);
      state.searchCounts = counts;
      elements.fileStatus.textContent = `${file.name} · ${formatBytes(file.size)}`;
      renderAll();
      requestAnimationFrame(fitGraph);
    } catch (error) {
      state = null;
      selected = null;
      expanded.clear();
      graphLayout = null;
      elements.fileStatus.textContent = `載入失敗：${error.message}`;
      elements.treeSummary.textContent = "0 nodes";
      renderTree();
      renderNavigation();
    } finally {
      elements.file.disabled = false;
    }
  }

  elements.file.addEventListener("change", event => {
    const file = event.target.files && event.target.files[0];
    if (file) loadFile(file);
  });

  elements.showSkipped.addEventListener("change", () => {
    if (!state) return;
    if (!showSkipped() && selected.synthetic) selected = selected.parent;
    renderAll();
  });

  elements.toggleCurrent.addEventListener("click", () => {
    if (!selected) return;
    if (expanded.has(selected)) expanded.delete(selected);
    else expanded.add(selected);
    renderAll();
  });

  elements.ttReference.addEventListener("click", () => {
    const source = selected && ttSourceNode(selected);
    if (source) selectNode(source);
  });

  elements.goRoot.addEventListener("click", () => selectNode(state && state.searchRoot));
  elements.goParent.addEventListener("click", () => {
    if (selected && selected !== state.searchRoot) selectNode(selected.parent);
  });
  elements.goChild.addEventListener("click", () => {
    const child = selected && visibleChildren(selected, showSkipped())[0];
    if (child) selectNode(child);
  });
  elements.goPrev.addEventListener("click", () => selectNode(siblingAtOffset(selected, -1)));
  elements.goNext.addEventListener("click", () => selectNode(siblingAtOffset(selected, 1)));
  elements.expandAll.addEventListener("click", expandAllVisibleNodes);
  elements.zoomOut.addEventListener("click", () => zoomGraphAtCenter(1 / 1.2));
  elements.zoomIn.addEventListener("click", () => zoomGraphAtCenter(1.2));
  elements.fitGraph.addEventListener("click", fitGraph);
  elements.resetView.addEventListener("click", resetGraphView);

  document.addEventListener("keydown", event => {
    if (!state || !selected) return;
    const target = event.target;
    if (target && (["INPUT", "BUTTON", "SELECT", "TEXTAREA"].includes(target.tagName) || target.isContentEditable)) return;
    let destination = null;
    if (event.key === "ArrowUp" && selected !== state.searchRoot) destination = selected.parent;
    if (event.key === "ArrowDown") destination = visibleChildren(selected, showSkipped())[0] || null;
    if (event.key === "ArrowLeft") destination = siblingAtOffset(selected, -1);
    if (event.key === "ArrowRight") destination = siblingAtOffset(selected, 1);
    if (event.key === "Home") destination = state.searchRoot;
    if (!destination) return;
    event.preventDefault();
    selectNode(destination);
  });

  // Paint an empty board before a file is selected.
  for (let i = 0; i < BOARD_SIZE * BOARD_SIZE; i++) {
    const tile = document.createElement("div");
    tile.className = "tile";
    elements.board.appendChild(tile);
  }
  renderNavigation();
}

if (typeof document !== "undefined") bootViewer();

if (typeof module !== "undefined" && module.exports) {
  module.exports = {
    actionName,
    applyAction,
    applySlide,
    boardFor,
    buildTraceNodeIndex,
    commentValue,
    dfsVisitOrderOf,
    expandVisibleSubtreeBatch,
    findSearchRoot,
    isSymmetryTtHit,
    isTtHit,
    layoutVisibleTree,
    materializeSkippedNodes,
    parseSgf,
    prepareSgf,
    solvedByOf,
    statusOf,
    traceNodeIdOf,
    ttSourceNodeIdOf,
  };
}
