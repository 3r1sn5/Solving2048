"use strict";

(() => {
  const PAGE_SIZE = 100;
  const directions = ["up", "right", "down", "left"];
  const state = {
    data: null,
    certificates: [],
    certificateById: new Map(),
    occurrences: [],
    occurrencesByCertificate: new Map(),
    filtered: [],
    page: 0,
    browserMode: "certificates",
    selectedCertificate: null,
    selectedOccurrence: null,
  };

  const $ = id => document.getElementById(id);
  const elements = {
    file: $("json-file"), fileStatus: $("file-status"), dropZone: $("drop-zone"), demo: $("demo"),
    summary: $("summary"), filterSummary: $("filter-summary"), search: $("search"),
    kindFilter: $("kind-filter"), phaseFilter: $("phase-filter"), cellsFilter: $("cells-filter"),
    sortFilter: $("sort-filter"), referencedFilter: $("referenced-filter"), clearFilters: $("clear-filters"),
    certificateList: $("certificate-list"), prevPage: $("prev-page"), nextPage: $("next-page"),
    pageLabel: $("page-label"), selectedLabel: $("selected-label"), kindBadge: $("kind-badge"),
    browserMode: $("browser-mode"), previousCertificate: $("previous-certificate"),
    nextCertificate: $("next-certificate"), explorerTitle: $("explorer-title"), boardContext: $("board-context"),
    patternBoard: $("pattern-board"), candidateBoard: $("candidate-board"),
    certificateDetails: $("certificate-details"), occurrenceDetails: $("occurrence-details"),
    occurrenceLabel: $("occurrence-label"), proofChain: $("proof-chain"),
    chainSummary: $("chain-summary"), occurrenceList: $("occurrence-list"),
    occurrenceSummary: $("occurrence-summary"), cellDistribution: $("cell-distribution"),
    runStatistics: $("run-statistics"),
  };

  function rowsToObjects(columns, rows) {
    if (!Array.isArray(columns) || !Array.isArray(rows)) throw new Error("columnar data is missing");
    return rows.map((row, rowIndex) => {
      if (!Array.isArray(row) || row.length !== columns.length) {
        throw new Error(`row ${rowIndex} does not match its columns`);
      }
      const object = {};
      columns.forEach((name, index) => { object[name] = row[index]; });
      return object;
    });
  }

  function maskNumber(value) {
    const parsed = Number.parseInt(value, 16);
    return Number.isFinite(parsed) ? parsed : 0;
  }

  function popcount(value) {
    let count = 0;
    for (let bits = value >>> 0; bits; bits >>>= 1) count += bits & 1;
    return count;
  }

  function compact(value) {
    const number = Number(value);
    if (!Number.isFinite(number)) return String(value ?? "—");
    return new Intl.NumberFormat("en", { notation: number >= 10000 ? "compact" : "standard", maximumFractionDigits: 2 }).format(number);
  }

  function fullNumber(value) {
    const number = Number(value);
    return Number.isFinite(number) ? new Intl.NumberFormat("en").format(number) : String(value ?? "—");
  }

  function kindLabel(kind) {
    return ({ terminal: "Terminal", fixed_slide: "Fixed slide", universal_placement: "Universal placement" })[kind] || kind;
  }

  function argumentLabel(certificate) {
    if (certificate.kind === "terminal") return `target @${certificate.argument}`;
    if (certificate.kind === "fixed_slide") return directions[certificate.argument] || `slide ${certificate.argument}`;
    return "all placements";
  }

  function boardTiles(raw) {
    let bits;
    try { bits = BigInt(raw); } catch { bits = 0n; }
    const values = [];
    for (let position = 0; position < 16; ++position) {
      const rank = Number((bits >> BigInt(position * 4)) & 0xfn);
      values.push(rank === 0 ? 0 : 2 ** rank);
    }
    return values;
  }

  function renderBoard(target, raw, maskHex, patternOnly = false) {
    target.innerHTML = "";
    const tiles = boardTiles(raw || "0x0");
    const mask = maskNumber(maskHex || "0x0");
    for (let position = 0; position < 16; ++position) {
      const inside = (mask & (1 << position)) !== 0;
      const cell = document.createElement("div");
      cell.className = `board-cell${inside ? " zone" : ""}${inside && tiles[position] === 0 ? " required-empty" : ""}`;
      const index = document.createElement("span");
      index.className = "position";
      index.textContent = position;
      const value = document.createElement("span");
      value.textContent = patternOnly && !inside ? "＊" : tiles[position] === 0 ? "·" : tiles[position];
      cell.append(index, value);
      target.appendChild(cell);
    }
  }

  function detail(label, value, title = "") {
    const wrapper = document.createElement("div");
    wrapper.className = "detail";
    const dt = document.createElement("dt");
    dt.textContent = label;
    const dd = document.createElement("dd");
    dd.textContent = value ?? "—";
    dd.title = title || dd.textContent;
    wrapper.append(dt, dd);
    return wrapper;
  }

  function renderDetails(target, values) {
    target.innerHTML = "";
    for (const [label, value, title] of values) target.appendChild(detail(label, value, title));
  }

  function makeSummaryCard(label, value, note) {
    const fragment = $("summary-card-template").content.cloneNode(true);
    fragment.querySelector("span").textContent = label;
    fragment.querySelector("strong").textContent = value;
    fragment.querySelector("small").textContent = note;
    return fragment;
  }

  function renderSummary() {
    const data = state.data;
    const stats = data.statistics;
    elements.summary.innerHTML = "";
    const hits = data.hit_statistics || {};
    const occurrenceExport = data.occurrence_export || {
      total: data.occurrences?.length || 0,
      exported: data.occurrences?.length || 0,
      truncated: false,
    };
    const cards = [
      ["Winning tile", fullNumber(data.rz.winning_tile), "solver target"],
      ["Certificates", fullNumber(data.rz.record_count), `${fullNumber(data.rz.exported_records)} in JSON`],
      ["SGF annotations", fullNumber(occurrenceExport.total), occurrenceExport.truncated
        ? `${fullNumber(occurrenceExport.exported)} sampled` : `${fullNumber(occurrenceExport.exported)} exported`],
      ["Exact TT hits", fullNumber(hits.total || 0), "normal + active-pruning"],
      ["Reused certificates", fullNumber(hits.certificates_reused || 0), `${percentage(hits.certificates_reused || 0, data.rz.record_count)} of table`],
      ["Proof depth", fullNumber(stats.maximum_depth), `${fullNumber(stats.unique_masks)} unique masks`],
    ];
    for (const card of cards) elements.summary.appendChild(makeSummaryCard(...card));
  }

  function percentage(value, total) {
    return total ? `${(100 * Number(value) / Number(total)).toFixed(1)}%` : "0%";
  }

  function applyFilters() {
    const query = elements.search.value.trim().toLowerCase();
    const kind = elements.kindFilter.value;
    const phase = elements.phaseFilter.value;
    const cells = elements.cellsFilter.value;
    const referencedOnly = elements.referencedFilter.checked;
    state.filtered = state.certificates.filter(certificate => {
      const matchesText = !query || String(certificate.id).includes(query) ||
        certificate.mask.toLowerCase().includes(query) || certificate.masked_board.toLowerCase().includes(query);
      return matchesText && (!kind || certificate.kind === kind) && (!phase || certificate.phase === phase) &&
        (!cells || certificate.cells === Number(cells)) && (!referencedOnly || certificate.occurrences > 0);
    });
    if (state.browserMode === "reused") state.filtered = state.filtered.filter(certificate => certificate.hit_count > 0);
    const sort = state.browserMode === "reused" ? "hits" : elements.sortFilter.value;
    state.filtered.sort((left, right) => {
      if (sort === "hits") return right.hit_count - left.hit_count || left.id - right.id;
      if (sort === "cells") return left.cells - right.cells || left.id - right.id;
      if (sort === "depth") return right.depth - left.depth || left.id - right.id;
      return left.id - right.id;
    });
    state.page = 0;
    renderCertificateList();
    updateNavigation();
  }

  function renderCertificateList() {
    elements.certificateList.innerHTML = "";
    const pages = Math.ceil(state.filtered.length / PAGE_SIZE);
    if (state.page >= pages) state.page = Math.max(0, pages - 1);
    const start = state.page * PAGE_SIZE;
    for (const certificate of state.filtered.slice(start, start + PAGE_SIZE)) {
      const button = document.createElement("button");
      button.type = "button";
      button.className = `certificate-row${state.selectedCertificate?.id === certificate.id ? " selected" : ""}`;
      const id = document.createElement("span");
      id.className = "id";
      id.textContent = `#${certificate.id}`;
      const description = document.createElement("span");
      description.className = "description";
      const title = document.createElement("strong");
      title.textContent = `${kindLabel(certificate.kind)} · ${certificate.phase}`;
      const pattern = document.createElement("small");
      pattern.textContent = `${certificate.mask} / ${certificate.masked_board}`;
      description.append(title, pattern);
      const count = document.createElement("span");
      count.className = `row-stat${certificate.hit_count > 0 ? " reused" : ""}`;
      count.textContent = certificate.hit_count > 0 ? `${compact(certificate.hit_count)} hits` : `${certificate.cells} cells`;
      button.append(id, description, count);
      button.addEventListener("click", () => selectCertificate(certificate.id));
      elements.certificateList.appendChild(button);
    }
    if (!state.filtered.length) elements.certificateList.innerHTML = '<div class="empty-note">沒有符合篩選條件的 certificate</div>';
    elements.explorerTitle.textContent = state.browserMode === "reused" ? "Top reused RZones" : "Certificates";
    elements.filterSummary.textContent = `${fullNumber(state.filtered.length)} / ${fullNumber(state.certificates.length)} exported records`;
    elements.pageLabel.textContent = pages ? `${state.page + 1} / ${pages}` : "0 / 0";
    elements.prevPage.disabled = state.page === 0;
    elements.nextPage.disabled = state.page + 1 >= pages;
  }

  function updateNavigation() {
    const index = state.selectedCertificate ? state.filtered.findIndex(item => item.id === state.selectedCertificate.id) : -1;
    elements.previousCertificate.disabled = index <= 0;
    elements.nextCertificate.disabled = index < 0 || index + 1 >= state.filtered.length;
  }

  function stepCertificate(delta) {
    if (!state.selectedCertificate) return;
    const index = state.filtered.findIndex(item => item.id === state.selectedCertificate.id);
    const next = state.filtered[index + delta];
    if (next) selectCertificate(next.id);
  }

  function selectCertificate(id, keepOccurrence = false) {
    const certificate = state.certificateById.get(Number(id));
    if (!certificate) return;
    state.selectedCertificate = certificate;
    if (!keepOccurrence) state.selectedOccurrence = null;
    renderCertificateList();
    renderSelectedCertificate();
    updateNavigation();
  }

  function renderSelectedCertificate() {
    const certificate = state.selectedCertificate;
    if (!certificate) return;
    elements.selectedLabel.textContent = `Certificate #${certificate.id} · ${certificate.phase}`;
    elements.kindBadge.textContent = kindLabel(certificate.kind);
    elements.kindBadge.className = `pill ${certificate.kind}`;
    elements.boardContext.textContent = `${certificate.cells} cells · ${fullNumber(certificate.hit_count)} exact TT hits`;
    renderBoard(elements.patternBoard, certificate.masked_board, certificate.mask, true);
    renderDetails(elements.certificateDetails, [
      ["Certificate ID", `#${certificate.id}`], ["Kind", kindLabel(certificate.kind)],
      ["Phase", certificate.phase], ["Mask", certificate.mask], ["Cells", String(certificate.cells)],
      ["Masked board", certificate.masked_board], ["Depth", String(certificate.depth)],
      ["Proof argument", argumentLabel(certificate)], ["Child", certificate.child ? `#${certificate.child}` : "terminal"],
      ["Exact TT hits", fullNumber(certificate.hit_count)],
      ["Share of all hits", percentage(certificate.hit_count, state.data.hit_statistics?.total || 0)],
      ["Sampled occurrences", fullNumber(certificate.occurrences)],
    ]);
    renderProofChain();
    if (!state.selectedOccurrence) {
      const occurrences = state.occurrencesByCertificate.get(certificate.id) || [];
      if (occurrences.length) state.selectedOccurrence = occurrences[0];
    }
    renderOccurrences();
    if (state.selectedOccurrence) renderOccurrenceBoard(state.selectedOccurrence);
    else {
      renderBoard(elements.candidateBoard, "0x0", "0x0");
      elements.occurrenceLabel.textContent = "選擇一個 SGF occurrence";
      elements.occurrenceDetails.innerHTML = "";
    }
  }

  function renderProofChain() {
    elements.proofChain.innerHTML = "";
    const chain = [];
    const seen = new Set();
    let current = state.selectedCertificate;
    while (current && !seen.has(current.id)) {
      chain.push(current);
      seen.add(current.id);
      current = current.child ? state.certificateById.get(current.child) : null;
    }
    chain.forEach((certificate, index) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = `chain-row${index === 0 ? " selected" : ""}`;
      const step = document.createElement("span");
      step.className = "chain-index";
      step.textContent = index + 1;
      const main = document.createElement("span");
      main.className = "chain-main";
      const title = document.createElement("strong");
      title.textContent = `#${certificate.id} · ${kindLabel(certificate.kind)}`;
      const subtitle = document.createElement("small");
      subtitle.textContent = `${certificate.phase} · ${certificate.cells} cells · depth ${certificate.depth}`;
      main.append(title, subtitle);
      const action = document.createElement("span");
      action.className = "chain-action";
      action.textContent = argumentLabel(certificate);
      button.append(step, main, action);
      button.addEventListener("click", () => selectCertificate(certificate.id));
      elements.proofChain.appendChild(button);
    });
    elements.chainSummary.textContent = `${chain.length} steps`;
  }

  function renderOccurrences() {
    const occurrences = state.occurrencesByCertificate.get(state.selectedCertificate.id) || [];
    elements.occurrenceList.innerHTML = "";
    occurrences.forEach(occurrence => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = `occurrence-row${state.selectedOccurrence === occurrence ? " selected" : ""}`;
      const node = document.createElement("strong");
      node.textContent = `Run ${occurrence.run}`;
      const main = document.createElement("span");
      const title = document.createElement("strong");
      title.textContent = `Node #${occurrence.node} · ${occurrence.action}`;
      const subtitle = document.createElement("small");
      subtitle.textContent = `${occurrence.phase} · depth ${occurrence.depth} · transform ${occurrence.transform}`;
      main.append(title, subtitle);
      const type = document.createElement("span");
      const hit = occurrence.solved_by === "RZONE_TT";
      type.className = `occurrence-type${hit ? " hit" : ""}`;
      type.textContent = hit ? "HIT" : "PROOF";
      button.append(node, main, type);
      button.addEventListener("click", () => {
        state.selectedOccurrence = occurrence;
        renderOccurrences();
        renderOccurrenceBoard(occurrence);
      });
      elements.occurrenceList.appendChild(button);
    });
    if (!occurrences.length) elements.occurrenceList.innerHTML = '<div class="empty-note">抽樣資料中沒有此 certificate 的 SGF occurrence；hit count 仍是完整 SGF 的精確值。</div>';
    elements.occurrenceSummary.textContent = `${fullNumber(occurrences.length)} sampled · ${fullNumber(state.selectedCertificate.hit_count)} exact hits`;
  }

  function renderOccurrenceBoard(occurrence) {
    renderBoard(elements.candidateBoard, occurrence.board, occurrence.candidate_mask);
    elements.occurrenceLabel.textContent = `Run ${occurrence.run} · node #${occurrence.node}`;
    const run = state.data.runs[occurrence.run - 1];
    renderDetails(elements.occurrenceDetails, [
      ["Run", String(occurrence.run)], ["Node", `#${occurrence.node}`],
      ["Parent", occurrence.parent ? `#${occurrence.parent}` : "root"], ["DFS depth", String(occurrence.depth)],
      ["Action", occurrence.action], ["Phase", occurrence.phase], ["Outcome", occurrence.outcome],
      ["Solved by", occurrence.solved_by], ["Transform", String(occurrence.transform)],
      ["Candidate mask", occurrence.candidate_mask], ["Board", occurrence.board],
      ["SGF", run?.path || "—", run?.path || ""],
    ]);
  }

  function renderDistribution() {
    elements.cellDistribution.innerHTML = "";
    const counts = state.data.statistics.mask_cells || [];
    const hits = state.data.hit_statistics?.mask_cells || [];
    const maximum = Math.max(1, ...counts);
    const maximumHits = Math.max(1, ...hits);
    for (let cells = 1; cells <= 16; ++cells) {
      const row = document.createElement("div");
      row.className = "distribution-row";
      const label = document.createElement("strong");
      label.textContent = cells;
      const bars = document.createElement("div");
      bars.className = "distribution-bars";
      for (const [value, denominator, className] of [[counts[cells] || 0, maximum, "patterns"], [hits[cells] || 0, maximumHits, "hits"]]) {
        const track = document.createElement("div");
        track.className = "bar-track";
        const bar = document.createElement("div");
        bar.className = `bar ${className}`;
        bar.style.width = `${100 * value / denominator}%`;
        track.appendChild(bar);
        bars.appendChild(track);
      }
      const count = document.createElement("span");
      count.className = "distribution-values";
      count.textContent = `${compact(counts[cells] || 0)} / ${compact(hits[cells] || 0)}`;
      count.title = `${fullNumber(counts[cells] || 0)} certificates / ${fullNumber(hits[cells] || 0)} exact hits`;
      row.append(label, bars, count);
      elements.cellDistribution.appendChild(row);
    }
  }

  function renderRuns() {
    elements.runStatistics.innerHTML = "";
    if (!state.data.runs.length) {
      elements.runStatistics.innerHTML = '<div class="empty-note">export 時未提供 SGF 或 sequence</div>';
      return;
    }
    const preferred = [
      "player_nodes", "environment_nodes", "rzone_patterns_stored", "rzone_lookups",
      "rzone_player_hits", "rzone_environment_hits", "rzone_partial_hits", "rzone_full_board_hits",
      "rzone_slide_lift_attempts", "rzone_slide_lift_successes", "rzone_placement_lift_attempts",
      "rzone_placement_lift_successes", "rzone_placement_lift_blocked",
      "rzone_environment_all_win_without_rzone", "rzone_active_pruning_children_matched", "elapsed_seconds",
    ];
    for (const run of state.data.runs) {
      const card = document.createElement("article");
      card.className = "run-card";
      const title = document.createElement("div");
      title.className = "run-title";
      const name = document.createElement("strong");
      name.textContent = `Opening ${run.index} · ${run.outcome} · ${fullNumber(run.annotated_nodes)} annotations`;
      const path = document.createElement("span");
      path.textContent = run.path;
      path.title = run.path;
      title.append(name, path);
      const metrics = document.createElement("div");
      metrics.className = "run-metrics";
      const keys = preferred.filter(key => run.statistics[key] !== undefined);
      for (const key of keys) {
        const metric = document.createElement("div");
        metric.className = "run-metric";
        const label = document.createElement("small");
        label.textContent = key.replace(/^rzone_/, "").replaceAll("_", " ");
        const value = document.createElement("strong");
        value.textContent = fullNumber(run.statistics[key]);
        metric.append(label, value);
        metrics.appendChild(metric);
      }
      card.append(title, metrics);
      elements.runStatistics.appendChild(card);
    }
  }

  function loadData(data, label) {
    if (!data || data.schema_version !== 1 || !data.rz || !data.statistics) {
      throw new Error("不支援的 RZone viewer JSON schema");
    }
    const certificates = rowsToObjects(data.certificate_columns, data.certificates);
    const occurrences = rowsToObjects(data.occurrence_columns || [], data.occurrences || []);
    const byId = new Map();
    certificates.forEach(certificate => {
      certificate.id = Number(certificate.id);
      certificate.child = Number(certificate.child);
      certificate.depth = Number(certificate.depth);
      certificate.argument = Number(certificate.argument);
      certificate.hit_count = Number(certificate.hit_count || 0);
      certificate.cells = popcount(maskNumber(certificate.mask));
      certificate.occurrences = 0;
      byId.set(certificate.id, certificate);
    });
    const occurrenceMap = new Map();
    occurrences.forEach(occurrence => {
      for (const key of ["certificate", "run", "node", "parent", "depth", "transform"]) occurrence[key] = Number(occurrence[key]);
      if (!occurrenceMap.has(occurrence.certificate)) occurrenceMap.set(occurrence.certificate, []);
      occurrenceMap.get(occurrence.certificate).push(occurrence);
      const certificate = byId.get(occurrence.certificate);
      if (certificate) ++certificate.occurrences;
    });
    state.data = data;
    state.certificates = certificates;
    state.certificateById = byId;
    state.occurrences = occurrences;
    state.occurrencesByCertificate = occurrenceMap;
    state.selectedCertificate = null;
    state.selectedOccurrence = null;
    state.browserMode = "certificates";
    elements.search.value = "";
    elements.kindFilter.value = "";
    elements.phaseFilter.value = "";
    elements.sortFilter.value = "id";
    elements.referencedFilter.checked = false;
    for (const button of elements.browserMode.querySelectorAll("button")) {
      button.classList.toggle("active", button.dataset.mode === state.browserMode);
    }
    elements.fileStatus.textContent = `${label} · ${fullNumber(certificates.length)} exported`;
    elements.cellsFilter.innerHTML = '<option value="">全部</option>';
    for (let cells = 1; cells <= 16; ++cells) {
      if (!(data.statistics.mask_cells?.[cells] > 0)) continue;
      const option = document.createElement("option");
      option.value = cells;
      option.textContent = `${cells} (${fullNumber(data.statistics.mask_cells[cells])})`;
      elements.cellsFilter.appendChild(option);
    }
    renderSummary();
    renderDistribution();
    renderRuns();
    applyFilters();
    const initial = [...certificates].sort((left, right) => right.hit_count - left.hit_count || left.id - right.id)[0];
    if (initial) selectCertificate(initial.id);
  }

  async function loadFile(file) {
    const maximumBrowserPayload = 256 * 1024 * 1024;
    if (file.size > maximumBrowserPayload) {
      throw new Error(
        `JSON 大小為 ${(file.size / (1024 * 1024)).toFixed(1)} MiB，無法安全地載入瀏覽器。` +
        "請以新版 exporter 的預設 sampling 重新產生，或降低 --max-occurrences。"
      );
    }
    elements.fileStatus.textContent = `讀取 ${file.name}…`;
    const text = await file.text();
    loadData(JSON.parse(text), file.name);
  }

  function showError(error) {
    console.error(error);
    elements.fileStatus.textContent = `錯誤：${error.message}`;
  }

  const demo = {
    schema_version: 1,
    generator: "embedded demo",
    rz: { path: "demo.rz", winning_tile: 16, record_count: 3, payload_checksum: "0x0123456789abcdef", exported_records: 3, selection: "all" },
    statistics: { terminal: 1, fixed_slide: 1, universal_placement: 1, player: 2, environment: 1, unique_masks: 2, maximum_depth: 3, total_depth: 6, mask_cells: [0, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0] },
    certificate_columns: ["id", "kind", "phase", "mask", "masked_board", "child", "depth", "argument", "hit_count"],
    certificates: [[1, "terminal", "player", "0x0001", "0x0000000000000004", 0, 1, 0, 0], [2, "fixed_slide", "player", "0x0003", "0x0000000000000033", 1, 2, 3, 17], [3, "universal_placement", "environment", "0x0003", "0x0000000000000033", 2, 3, -1, 4]],
    runs: [{ index: 1, path: "demo.sgf", opening_board: "0x0000000000000011", outcome: "WIN", annotated_nodes: 1, statistics: { player_nodes: "42", environment_nodes: "21", rzone_patterns_stored: "3", rzone_lookups: "9", rzone_player_hits: "1", elapsed_seconds: "0.0042" } }],
    occurrence_columns: ["certificate", "run", "node", "parent", "depth", "phase", "board", "outcome", "solved_by", "transform", "candidate_mask", "action"],
    occurrences: [[2, 1, 12, 11, 4, "player", "0x0000000000002033", "WIN", "RZONE_TT", 0, "0x0003", "2@5"]],
    occurrence_export: { mode: "all_annotations", total: 1, exported: 1, limit: 50000, truncated: false, sampling: "none" },
    hit_statistics: { total: 21, certificates_reused: 2, player: 18, environment: 3, exact_orientation: 15, symmetry: 6, mask_cells: [0, 0, 21, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0] },
    top_hits: [[2, 17], [3, 4]],
  };

  elements.file.addEventListener("change", () => {
    const file = elements.file.files?.[0];
    if (file) loadFile(file).catch(showError);
  });
  for (const event of ["dragenter", "dragover"]) elements.dropZone.addEventListener(event, e => { e.preventDefault(); elements.dropZone.classList.add("dragging"); });
  for (const event of ["dragleave", "drop"]) elements.dropZone.addEventListener(event, e => { e.preventDefault(); elements.dropZone.classList.remove("dragging"); });
  elements.dropZone.addEventListener("drop", event => {
    const file = event.dataTransfer?.files?.[0];
    if (file) loadFile(file).catch(showError);
  });
  elements.demo.addEventListener("click", () => loadData(demo, "embedded demo"));
  for (const control of [elements.search, elements.kindFilter, elements.phaseFilter, elements.cellsFilter, elements.sortFilter, elements.referencedFilter]) {
    control.addEventListener(control === elements.search ? "input" : "change", applyFilters);
  }
  elements.clearFilters.addEventListener("click", () => {
    elements.search.value = "";
    elements.kindFilter.value = "";
    elements.phaseFilter.value = "";
    elements.cellsFilter.value = "";
    elements.sortFilter.value = state.browserMode === "reused" ? "hits" : "id";
    elements.referencedFilter.checked = false;
    applyFilters();
  });
  elements.prevPage.addEventListener("click", () => { if (state.page > 0) { --state.page; renderCertificateList(); } });
  elements.nextPage.addEventListener("click", () => { if ((state.page + 1) * PAGE_SIZE < state.filtered.length) { ++state.page; renderCertificateList(); } });
  elements.previousCertificate.addEventListener("click", () => stepCertificate(-1));
  elements.nextCertificate.addEventListener("click", () => stepCertificate(1));
  elements.browserMode.addEventListener("click", event => {
    const button = event.target.closest("button[data-mode]");
    if (!button) return;
    state.browserMode = button.dataset.mode;
    for (const candidate of elements.browserMode.querySelectorAll("button")) candidate.classList.toggle("active", candidate === button);
    if (state.browserMode === "reused") elements.sortFilter.value = "hits";
    applyFilters();
    if (!state.filtered.some(certificate => certificate.id === state.selectedCertificate?.id) && state.filtered[0]) selectCertificate(state.filtered[0].id);
  });

  renderBoard(elements.patternBoard, "0x0", "0x0", true);
  renderBoard(elements.candidateBoard, "0x0", "0x0");
  if (window.location.hash === "#demo") loadData(demo, "embedded demo");
})();
