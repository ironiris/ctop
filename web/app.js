"use strict";

const $ = (id) => document.getElementById(id);
const esc = (value) => String(value ?? "").replace(/[&<>"']/g, (ch) => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[ch]));
const validNumber = (value) => value !== null && value !== undefined && value !== "" && Number.isFinite(Number(value));
const number = (value, digits = 1) => validNumber(value) ? Number(value).toLocaleString(undefined, {maximumFractionDigits: digits, minimumFractionDigits: digits}) : "—";
const gib = (bytes, digits = 1) => validNumber(bytes) ? `${number(Number(bytes) / 1073741824, digits)}GB` : "—";
const percent = (value) => validNumber(value) ? `${number(value, 1)}%` : "—";
const MONITORS_KEY = "ctop-monitors-v1";
const LOCAL_PREFIX_KEY = "ctop-local-prefix-v1";
const DASHBOARD_ORDER_KEY = "ctop-dashboard-section-order-v1";
const DASHBOARD_SECTION_KEYS = ["gpu-list", "gpu", "system", "gpu-processes"];
let draggedDashboardSection = null;
let dashboardDropTarget = null;
function loadDashboardSectionOrder() {
  let stored;
  try { stored = JSON.parse(localStorage.getItem(DASHBOARD_ORDER_KEY) || "[]"); } catch (_) {}
  const valid = Array.isArray(stored) ? stored.filter((key, index) =>
    DASHBOARD_SECTION_KEYS.includes(key) && stored.indexOf(key) === index) : [];
  return [...valid, ...DASHBOARD_SECTION_KEYS.filter((key) => !valid.includes(key))];
}
let dashboardSectionOrder = loadDashboardSectionOrder();

function rate(bytes) {
  if (!validNumber(bytes)) return "—";
  let value = Math.max(0, Number(bytes));
  const units = ["B", "K", "M", "G", "T"];
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) { value /= 1024; unit++; }
  return `${number(value, unit <= 2 || value >= 100 ? 0 : 1)}${units[unit]}`;
}
function networkRate(bps) {
  if (!validNumber(bps)) return "—";
  const n = Number(bps);
  if (n >= 1e9) return `${number(n / 1e9)}G`;
  if (n >= 1e6) return `${Math.round(n / 1e6)}M`;
  if (n >= 1e3) return `${Math.round(n / 1e3)}K`;
  return `${Math.round(n)}b/s`;
}
function setText(id, text) { $(id).textContent = text; }
function applyDashboardSectionOrder() {
  if (draggedDashboardSection) return;
  const root = $("dashboard-sections");
  for (const key of dashboardSectionOrder) {
    const slot = [...root.children].find((child) => child.dataset.dashboardSection === key);
    if (slot) root.appendChild(slot);
  }
}
function saveDashboardSectionOrder() {
  const root = $("dashboard-sections");
  dashboardSectionOrder = [...root.children].map((child) => child.dataset.dashboardSection)
    .filter((key) => DASHBOARD_SECTION_KEYS.includes(key));
  try { localStorage.setItem(DASHBOARD_ORDER_KEY, JSON.stringify(dashboardSectionOrder)); } catch (_) {}
}

const HISTORY_MARKS = [400, 120, 30, 10, 0];
let sampleIntervalSeconds = 1.0;
function historyPosition(age) {
  const boundedAge = Math.max(0, Math.min(400, age));
  for (let i = 0; i < HISTORY_MARKS.length - 1; i++) {
    const older = HISTORY_MARKS[i], newer = HISTORY_MARKS[i + 1];
    if (boundedAge >= newer) {
      const within = (older - boundedAge) / (older - newer);
      return (i + within) / (HISTORY_MARKS.length - 1);
    }
  }
  return 1;
}

function axisRate(value, max) {
  const useM = max >= 1048576;
  const divisor = useM ? 1048576 : 1024;
  return `${Math.floor(Math.max(0, value) / divisor)}${useM ? "M/s" : "K/s"}`;
}
function axisNetworkRate(value, max) {
  const unit = max >= 1e9 ? [1e9, "Gbps"]
    : max >= 1e6 ? [1e6, "Mbps"]
    : max >= 1e3 ? [1e3, "Kbps"] : [1, "bps"];
  const scaled = Math.max(0, value) / unit[0];
  return `${number(scaled, scaled > 0 && scaled < 10 ? 1 : 0)}${unit[1]}`;
}

function drawChart(svgId, axisId, values, mode = "percent", fixedMax = null) {
  const svg = $(svgId), axis = $(axisId);
  const samples = (values || []).map((sample, index, all) => {
    if (sample && typeof sample === "object")
      return {age: Number(sample.age_seconds), value: Number(sample.value)};
    const age = sampleIntervalSeconds * (all.length - 1 - index);
    return {age, value: Number(sample)};
  }).filter((sample) => Number.isFinite(sample.age) && Number.isFinite(sample.value) && sample.age >= 0 && sample.age <= 400)
    .sort((a, b) => b.age - a.age);
  const latestAge = samples.length ? samples[samples.length - 1].age : 0;
  const finite = samples.map((sample) => sample.value);
  const max = mode === "disk" || mode === "network"
    ? Math.max(0, ...finite)
    : (fixedMax || Math.max(0, ...finite, 1));
  const scale = max > 0 ? max : 1;
  const ticks = [1, .75, .5, .25, 0];
  axis.innerHTML = ticks.map((t) => `<span>${mode === "disk" ? axisRate(max * t, max) : mode === "network" ? axisNetworkRate(max * t, max) : `${Math.round((fixedMax || 100) * t)}%`}</span>`).join("");
  const width = 640, left = 2, right = width - 2, top = 8, bottom = 80, height = bottom - top;
  let grid = "";
  for (const t of ticks) {
    const y = bottom - height * t;
    grid += `M${left} ${y.toFixed(1)}H${right}`;
  }
  const lineParts = [], areaParts = [], pointParts = [];
  let segment = [];
  const finishSegment = () => {
    if (!segment.length) return;
    const first = segment[0], last = segment[segment.length - 1];
    const path = segment.map((point, i) => `${i ? "L" : "M"}${point.x.toFixed(2)},${point.y.toFixed(2)}`).join("");
    lineParts.push(path);
    if (segment.length > 1) {
      const areaPath = `M${first.x.toFixed(2)},${bottom}L${first.x.toFixed(2)},${first.y.toFixed(2)}` +
        segment.slice(1).map((point) => `L${point.x.toFixed(2)},${point.y.toFixed(2)}`).join("") +
        `L${last.x.toFixed(2)},${bottom}Z`;
      areaParts.push(areaPath);
    } else {
      const point = segment[0];
      pointParts.push(`<circle class="sample-point" cx="${point.x.toFixed(2)}" cy="${point.y.toFixed(2)}" r="2.5"/>`);
    }
    segment = [];
  };
  const maxGap = Math.max(1.7, sampleIntervalSeconds * 1.5 + 0.1);
  for (const sample of samples) {
    const previous = segment[segment.length - 1];
    if (previous && previous.age - sample.age > maxGap) finishSegment();
    const relativeAge = sample.age - latestAge;
    const x = left + (right - left) * historyPosition(relativeAge);
    const y = bottom - Math.max(0, Math.min(1, sample.value / scale)) * height;
    segment.push({age: sample.age, x, y});
  }
  finishSegment();
  svg.innerHTML = `<path class="grid-lines" d="${grid}"/><path class="area" d="${areaParts.join("")}"/><path class="line" d="${lineParts.join("")}"/>${pointParts.join("")}`;
}

function normalizePrefix(value) {
  const prefix = String(value ?? "").trim().toUpperCase();
  return /^[A-Z]{1,3}$/.test(prefix) ? prefix : "";
}
function prefixForIndex(index) {
  let prefix = "";
  while (index >= 0) {
    prefix = String.fromCharCode(index % 26 + 65) + prefix;
    index = Math.floor(index / 26) - 1;
  }
  return prefix;
}
function nextAvailablePrefix(used) {
  for (let i = 0; i < 26 * 26 * 26; i++) {
    const candidate = prefixForIndex(i);
    if (!used.has(candidate)) return candidate;
  }
  return "";
}
function normalizeMetricsUrl(input) {
  const address = String(input).trim();
  const hasScheme = /^[a-z][a-z0-9+.-]*:\/\//i.test(address);
  let url;
  try {
    url = hasScheme
      ? new URL(address)
      : new URL(address.startsWith("//") ? address : `//${address}`, window.location.href);
  } catch (_) { throw new Error("Enter a host:port or HTTP(S) metrics address."); }
  if (url.protocol !== "http:" && url.protocol !== "https:")
    throw new Error("Metrics address must use HTTP or HTTPS.");
  if (url.username || url.password)
    throw new Error("Put credentials in the token query parameter, not in the URL authority.");
  const path = url.pathname.replace(/\/+$/, "");
  if (!/(?:^|\/)api\/(?:v1\/)?metrics$/i.test(path))
    url.pathname = `${path}/api/v1/metrics`;
  return url.href;
}
function endpointKey(address) {
  const url = new URL(address, window.location.href);
  return `${url.origin}${url.pathname}`;
}
function localEndpointKey() {
  return endpointKey(new URL("/api/v1/metrics", window.location.href).href);
}
function randomSourceId() {
  return `remote-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
}
function localPrefixFromStorage() {
  try { return normalizePrefix(localStorage.getItem(LOCAL_PREFIX_KEY)) || "A"; }
  catch (_) { return "A"; }
}
function loadRemoteSources(localPrefix) {
  let stored;
  try { stored = JSON.parse(localStorage.getItem(MONITORS_KEY) || "[]"); }
  catch (_) { stored = []; }
  if (!Array.isArray(stored)) return [];
  const used = new Set([localPrefix]);
  const endpoints = new Set([localEndpointKey()]);
  const ids = new Set(["local"]);
  const result = [];
  for (const item of stored) {
    try {
      const url = normalizeMetricsUrl(item?.url);
      const key = endpointKey(url);
      if (endpoints.has(key)) continue;
      let prefix = normalizePrefix(item?.prefix);
      if (!prefix || used.has(prefix)) prefix = nextAvailablePrefix(used);
      if (!prefix) continue;
      let id = /^[a-zA-Z0-9_-]{1,64}$/.test(item?.id || "") ? item.id : randomSourceId();
      while (ids.has(id)) id = randomSourceId();
      ids.add(id); used.add(prefix); endpoints.add(key);
      result.push({id, domId: `source-${id}`, prefix, url, token: String(item?.token || ""), local: false, data: null, error: "", errorCode: 0, gpuCollapsed: false, systemCollapsed: false});
    } catch (_) {}
  }
  return result;
}

const localSource = {id: "local", domId: "source-local", prefix: localPrefixFromStorage(), url: "/api/v1/metrics", local: true, data: null, error: "", errorCode: 0, gpuCollapsed: false, systemCollapsed: false};
let sources = [localSource, ...loadRemoteSources(localSource.prefix)];
let tokenRedeemed = false;
let pollTimer = null;
let pollRunning = false;
let pollAgain = false;

function monitorData(source, kind) {
  const base = `${source.domId}-${kind}`;
  if (kind === "gpu") {
    const collapsed = source.gpuCollapsed;
    const sectionTitle = sources.length > 1 ? `GPU(${source.prefix})` : "GPU";
    return `<section class="section source-section ${collapsed ? "source-section-collapsed" : ""}">
      <div class="section-heading"><h2 data-section-drag="gpu" title="Drag to reorder sections">${esc(sectionTitle)}</h2><div class="section-meta source-heading-meta"><span>Average across GPUs</span><button class="collapse-toggle" type="button" data-collapse-source="${esc(source.id)}" data-collapse-kind="gpu" aria-expanded="${!collapsed}" aria-label="${collapsed ? "Expand" : "Collapse"} ${esc(sectionTitle)} graphs" title="${collapsed ? "Expand" : "Collapse"} GPU graphs"><span aria-hidden="true"></span></button></div></div>
      <div class="source-section-body" ${collapsed ? "hidden" : ""}><div class="history-grid">
        <article class="metric-card history-card"><div class="metric-head"><span class="metric-label">Average VRAM usage</span></div>
          <div id="${base}-vram-value" class="metric-value">—</div>
          <div class="chart-wrap"><div id="${base}-vram-axis" class="y-axis"></div><svg id="${base}-vram-chart" class="chart" viewBox="0 0 640 84" preserveAspectRatio="none" aria-label="${esc(source.prefix)} VRAM history"></svg></div>
          <div class="x-axis"><span>-400s</span><span>-120s</span><span>-30s</span><span>-10s</span><span>now</span></div>
        </article>
        <article class="metric-card history-card"><div class="metric-head"><span class="metric-label">Average GPU utilization</span></div>
          <div id="${base}-gpu-value" class="metric-value">—</div>
          <div class="chart-wrap"><div id="${base}-gpu-axis" class="y-axis"></div><svg id="${base}-gpu-chart" class="chart" viewBox="0 0 640 84" preserveAspectRatio="none" aria-label="${esc(source.prefix)} GPU utilization history"></svg></div>
          <div class="x-axis"><span>-400s</span><span>-120s</span><span>-30s</span><span>-10s</span><span>now</span></div>
        </article>
      </div></div></section>`;
  }
  const collapsed = source.systemCollapsed;
  const sectionTitle = sources.length > 1 ? `SYSTEM(${source.prefix})` : "SYSTEM";
  const card = (metric, label, icon, detailLabel) => `<article class="metric-card" data-metric="${metric}">
    <div class="metric-head"><span class="metric-label"><i class="metric-icon ${icon}"></i>${label}</span><span id="${base}-${metric}-detail" class="metric-info">—</span></div>
    <div id="${base}-${metric}-value" class="metric-value">—</div>
    <div class="chart-wrap"><div id="${base}-${metric}-axis" class="y-axis"></div><svg id="${base}-${metric}-chart" class="chart" viewBox="0 0 640 84" preserveAspectRatio="none" aria-label="${esc(source.prefix)} ${detailLabel} history"></svg></div>
    <div class="x-axis"><span>-400s</span><span>-120s</span><span>-30s</span><span>-10s</span><span>now</span></div>
  </article>`;
  return `<section class="section source-section ${collapsed ? "source-section-collapsed" : ""}">
    <div class="section-heading"><h2 data-section-drag="system" title="Drag to reorder sections">${esc(sectionTitle)}</h2><div class="section-meta source-heading-meta"><span>400-second history</span><button class="collapse-toggle" type="button" data-collapse-source="${esc(source.id)}" data-collapse-kind="system" aria-expanded="${!collapsed}" aria-label="${collapsed ? "Expand" : "Collapse"} ${esc(sectionTitle)}" title="${collapsed ? "Expand" : "Collapse"} system history"><span aria-hidden="true"></span></button></div></div>
    <div class="source-section-body" ${collapsed ? "hidden" : ""}><div class="metric-grid">
      ${card("ram", "RAM", "ram-icon", "RAM")}
      ${card("swap", "SWAP", "swap-icon", "SWAP")}
      ${card("disk", "DISK", "disk-icon", "disk I/O")}
      ${card("net", "NET", "net-icon", "network")}
    </div></div></section>`;
}

function renderSourceGroups() {
  $("gpu-source-groups").innerHTML = sources.map((source) => monitorData(source, "gpu")).join("");
  $("system-source-groups").innerHTML = sources.map((source) => monitorData(source, "system")).join("");
  for (const source of sources) if (source.data) renderSourceMetrics(source);
}
function renderSourceMetrics(source) {
  const data = source.data;
  sampleIntervalSeconds = validNumber(data.sample_interval) && Number(data.sample_interval) > 0
    ? Number(data.sample_interval) : 1.0;
  const base = `${source.domId}-system`;
  const ram = data.ram || {}, swap = data.swap || {}, disk = data.disk || {}, net = data.network || {};
  setText(`${base}-ram-value`, percent(ram.percent));
  setText(`${base}-ram-detail`, `(${gib(ram.used)}/${gib(ram.total)})`);
  setText(`${base}-swap-value`, percent(swap.percent));
  setText(`${base}-swap-detail`, `(${gib(swap.used)}/${gib(swap.total)})`);
  const diskTotal = Number(disk.read_bps || 0) + Number(disk.write_bps || 0);
  setText(`${base}-disk-value`, disk.valid ? rate(diskTotal) : "—");
  setText(`${base}-disk-detail`, `(${disk.capacity_valid ? `${gib(disk.used)}/${gib(disk.total)}` : "Capacity unavailable"} · R:${rate(disk.read_bps)} W:${rate(disk.write_bps)})`);
  setText(`${base}-net-value`, net.valid ? networkRate(net.total_bps) : "—");
  setText(`${base}-net-detail`, `(Tot:${net.valid ? networkRate(net.total_bps) : "—"} / ↓${networkRate(net.rx_bps)} ↑${networkRate(net.tx_bps)})`);
  const history = data.history || {};
  drawChart(`${base}-ram-chart`, `${base}-ram-axis`, history.ram, "percent", 100);
  drawChart(`${base}-swap-chart`, `${base}-swap-axis`, history.swap, "percent", 100);
  drawChart(`${base}-disk-chart`, `${base}-disk-axis`, history.disk, "disk");
  drawChart(`${base}-net-chart`, `${base}-net-axis`, history.network, "network");

  const gpuBase = `${source.domId}-gpu`;
  setText(`${gpuBase}-vram-value`, percent(data.averages?.vram_percent));
  setText(`${gpuBase}-gpu-value`, percent(data.averages?.gpu_percent));
  drawChart(`${gpuBase}-vram-chart`, `${gpuBase}-vram-axis`, history.vram, "percent", 100);
  drawChart(`${gpuBase}-gpu-chart`, `${gpuBase}-gpu-axis`, history.gpu, "percent", 100);
}

function renderGpuRows() {
  const available = sources.filter((source) => source.data);
  const devices = available.flatMap((source) => (source.data.gpus || []).map((gpu) => ({source, gpu})));
  const monitorSummary = sources.length > 1 ? ` · ${available.length}/${sources.length} monitors` : "";
  setText("gpu-count", `${devices.length} ${devices.length === 1 ? "device" : "devices"}${monitorSummary}`);
  const root = $("gpu-rows");
  if (!devices.length) {
    root.innerHTML = `<tr><td colspan="7" class="empty-row">Waiting for GPU metrics…</td></tr>`;
    return;
  }
  root.innerHTML = devices.map(({source, gpu}) => {
    const vramPercent = gpu.memory_valid && Number(gpu.memory_total) > 0
      ? 100 * Number(gpu.memory_used) / Number(gpu.memory_total) : null;
    const gpuPercent = gpu.utilization_valid && validNumber(gpu.utilization)
      ? Number(gpu.utilization) : null;
    const vramWidth = Math.max(0, Math.min(100, vramPercent ?? 0));
    const gpuWidth = Math.max(0, Math.min(100, gpuPercent ?? 0));
    const power = `${gpu.power_valid ? `${number(gpu.power)}W` : "—"}/${gpu.power_limit_valid ? `${number(gpu.power_limit)}W` : "—"}`;
    const vram = gpu.memory_valid
      ? `${gib(gpu.memory_used)}/${gib(gpu.memory_total)}${vramPercent === null ? "" : ` (${percent(vramPercent)})`}` : "—";
    return `<tr>
      <td class="mono">${sources.length > 1 ? esc(source.prefix) : ""}${esc(gpu.index)}</td><td>${esc(gpu.name || `GPU ${gpu.index}`)}</td>
      <td class="mono">${gpu.temperature_valid ? `${number(gpu.temperature, 0)}°C` : "—"}</td>
      <td class="mono">${power}</td><td class="mono">${vram}</td>
      <td class="num mono">${gpuPercent === null ? "—" : percent(gpuPercent)}</td>
      <td class="gpu-meter-cell"><div class="gpu-usage-meter" role="img" aria-label="VRAM ${vramPercent === null ? "unavailable" : percent(vramPercent)}, GPU ${gpuPercent === null ? "unavailable" : percent(gpuPercent)}">
        <div class="gpu-meter-half vram-meter"><span style="width:${vramWidth}%"></span></div><div class="gpu-meter-divider"></div>
        <div class="gpu-meter-half gpu-meter"><span style="width:${gpuWidth}%"></span></div></div></td></tr>`;
  }).join("");
}

function renderProcesses() {
  const processes = sources.flatMap((source) => (source.data?.processes || []).map((process) => ({source, process})));
  const monitorSummary = sources.length > 1 ? ` · ${sources.filter((source) => source.data).length}/${sources.length} monitors` : "";
  setText("process-count", `${processes.length} ${processes.length === 1 ? "process" : "processes"}${monitorSummary}`);
  const body = $("process-rows");
  if (!processes.length) {
    body.innerHTML = `<tr><td colspan="9" class="empty-row">No GPU processes reported by any monitor.</td></tr>`;
    return;
  }
  body.innerHTML = processes.map(({source, process: p}) => `<tr>
    <td class="mono">${sources.length > 1 ? esc(source.prefix) : ""}${esc(p.gpu)}</td><td class="pid">${esc(p.pid)}</td><td class="process-user">${esc(p.user || "?")}</td>
    <td class="num mono">${p.gpu_memory_valid ? gib(p.gpu_memory) : "—"}</td>
    <td class="num mono">${p.sm_valid ? percent(p.sm) : "—"}</td><td class="num mono">${p.cpu_valid ? percent(p.cpu) : "—"}</td>
    <td class="num mono">${p.memory_valid ? percent(p.memory) : "—"}</td><td class="mono">${esc(p.elapsed || "—")}</td>
    <td class="command" title="${esc(p.command)}">${esc(p.command)}</td></tr>`).join("");
}

function saveSources() {
  try {
    localStorage.setItem(MONITORS_KEY, JSON.stringify(sources.filter((source) => !source.local)
      .map(({id, prefix, url, token}) => ({id, prefix, url, token}))));
    localStorage.setItem(LOCAL_PREFIX_KEY, localSource.prefix);
  } catch (_) {}
}
function usedPrefixes(exceptSource = null) {
  return new Set(sources.filter((source) => source !== exceptSource).map((source) => source.prefix));
}
function setFormMessage(message, error = false) {
  const node = $("monitor-form-message");
  node.textContent = message;
  node.hidden = !message;
  node.classList.toggle("error", error);
}
function displayMetricsAddress(address) {
  try {
    const url = new URL(address, window.location.href);
    if (url.searchParams.has("token")) url.searchParams.set("token", "REDACTED");
    return url.href;
  } catch (_) { return address; }
}
function renderMonitorList() {
  const root = $("monitor-list");
  const remotes = sources.filter((source) => !source.local);
  if (!remotes.length) {
    root.innerHTML = `<li class="monitor-empty">No additional monitors.</li>`;
    return;
  }
  root.innerHTML = remotes.map((source) => `<li class="monitor-item">
    <span class="monitor-prefix">Prefix ${esc(source.prefix)}</span>
    <span class="monitor-address" title="${esc(displayMetricsAddress(source.url))}">${esc(displayMetricsAddress(source.url))}</span>
    <span class="monitor-state ${source.error ? "offline" : ""}">${source.error ? esc(source.error) : source.data ? "Connected" : "Connecting"}</span>
    <button type="button" class="monitor-remove" data-remove-source="${esc(source.id)}" aria-label="Remove monitor ${esc(source.prefix)}" title="Remove monitor">×</button>
  </li>`).join("");
}
function updatePrefixSuggestion() {
  $("monitor-prefix").value = nextAvailablePrefix(usedPrefixes());
}
function renderDashboard() {
  $("local-prefix-control").hidden = sources.length <= 1;
  renderGpuRows();
  renderProcesses();
  if (!draggedDashboardSection && !sectionDragCandidate) renderSourceGroups();
  applyDashboardSectionOrder();
  renderMonitorList();
}

function setConnected() {
  $("live-pill").classList.remove("pending", "error");
  $("live-label").textContent = "LIVE";
  const note = $("connection");
  note.hidden = false;
  note.textContent = "Connected";
}
function setStatus(message) {
  $("live-pill").classList.add("error");
  $("live-label").textContent = message;
  $("connection").hidden = true;
}
async function handleUnauthorized() {
  const stored = readStoredToken();
  if (stored && !tokenRedeemed) {
    tokenRedeemed = true;
    try {
      const attempt = await fetch(`/login?token=${encodeURIComponent(stored)}`, {cache: "no-store", redirect: "follow"});
      if (attempt.ok || attempt.status === 302) { window.location.replace("/"); return; }
    } catch (_) {}
  }
  const overlay = $("sign-in");
  overlay.hidden = false;
  setStatus("Sign in required");
  const field = $("sign-in-token");
  field.value = readStoredToken();
  field.focus();
}
function readStoredToken() {
  try { return localStorage.getItem("ctop-token") || ""; } catch (_) { return ""; }
}

async function poll() {
  if (pollRunning) { pollAgain = true; return; }
  pollRunning = true;
  try {
    const currentSources = [...sources];
    let localUnauthorized = false;
    await Promise.all(currentSources.map(async (source) => {
      try {
        const controller = new AbortController();
        const timeout = window.setTimeout(() => controller.abort(), 5000);
        let response;
        const options = {mode: source.local ? "same-origin" : "cors", signal: controller.signal};
        if (!source.local && source.token) options.headers = {Authorization: `Bearer ${source.token}`};
        try {
          response = await fetch(source.url, options);
        } finally {
          window.clearTimeout(timeout);
        }
        if (response.status === 401 && source.local) localUnauthorized = true;
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        const data = await response.json();
        if (!data || typeof data !== "object" || !Array.isArray(data.gpus) || !data.history)
          throw new Error("Invalid metrics response");
        source.data = data;
        source.error = "";
        source.errorCode = 0;
      } catch (error) {
        source.error = String(error?.message || "Connection failed").replace(/^Failed to fetch$/, "Connection failed");
        const match = source.error.match(/HTTP (\d+)/);
        source.errorCode = match ? Number(match[1]) : 0;
      }
    }));
    renderDashboard();
    const failed = currentSources.filter((source) => source.error).length;
    if (localUnauthorized) await handleUnauthorized();
    else if (failed) setStatus(`${failed} monitor${failed === 1 ? "" : "s"} offline · retrying`);
    else setConnected();
    if (localSource.data) {
      setText("updated", `Updated ${new Date().toLocaleTimeString()}`);
      setText("version", `v${esc(localSource.data.version || "")}`);
    }
  } finally {
    pollRunning = false;
    const delay = pollAgain ? 0 : 1000;
    pollAgain = false;
    pollTimer = window.setTimeout(poll, delay);
  }
}

$("local-prefix").value = localSource.prefix;
$("monitor-prefix").value = nextAvailablePrefix(usedPrefixes());
$("local-prefix").addEventListener("change", () => {
  const prefix = normalizePrefix($("local-prefix").value);
  if (!prefix) {
    $("local-prefix").value = localSource.prefix;
    setFormMessage("Prefixes must use 1–3 letters (A–Z).", true);
    return;
  }
  if (usedPrefixes(localSource).has(prefix)) {
    $("local-prefix").value = localSource.prefix;
    setFormMessage(`Prefix ${prefix} is already in use.`, true);
    return;
  }
  localSource.prefix = prefix;
  $("local-prefix").value = prefix;
  saveSources();
  updatePrefixSuggestion();
  setFormMessage("");
  renderDashboard();
});

$("monitor-url").addEventListener("blur", () => {
  const field = $("monitor-url");
  if (!field.value.trim()) return;
  try { field.value = normalizeMetricsUrl(field.value); } catch (_) {}
});

$("monitor-form").addEventListener("submit", (event) => {
  event.preventDefault();
  let url;
  try { url = normalizeMetricsUrl($("monitor-url").value); }
  catch (error) { setFormMessage(error.message, true); return; }
  const prefix = normalizePrefix($("monitor-prefix").value);
  if (!prefix) { setFormMessage("Prefixes must use 1–3 letters (A–Z).", true); return; }
  const token = $("monitor-token").value.trim();
  if (token && !/^[A-Za-z0-9._~-]{8,128}$/.test(token)) {
    setFormMessage("Token must be 8–128 characters using A–Z, a–z, 0–9, ., _, -, or ~.", true);
    return;
  }
  if (usedPrefixes().has(prefix)) { setFormMessage(`Prefix ${prefix} is already in use.`, true); return; }
  const key = endpointKey(url);
  if (sources.some((source) => endpointKey(source.url) === key)) {
    setFormMessage("This metrics endpoint is already monitored.", true);
    return;
  }
  const source = {id: randomSourceId(), domId: "", prefix, url, token, local: false, data: null, error: "", errorCode: 0, gpuCollapsed: false, systemCollapsed: false};
  source.domId = `source-${source.id}`;
  sources.push(source);
  saveSources();
  $("monitor-url").value = "";
  $("monitor-token").value = "";
  updatePrefixSuggestion();
  setFormMessage("");
  renderDashboard();
  if (pollTimer) clearTimeout(pollTimer);
  poll();
});

$("monitor-list").addEventListener("click", (event) => {
  const button = event.target.closest("[data-remove-source]");
  if (!button) return;
  sources = sources.filter((source) => source.id !== button.dataset.removeSource);
  saveSources();
  updatePrefixSuggestion();
  renderDashboard();
  setFormMessage("");
  if (pollTimer) clearTimeout(pollTimer);
  poll();
});

for (const groupId of ["gpu-source-groups", "system-source-groups"]) {
  $(groupId).addEventListener("click", (event) => {
    const button = event.target.closest("[data-collapse-source]");
    if (!button) return;
    const source = sources.find((item) => item.id === button.dataset.collapseSource);
    if (!source) return;
    if (button.dataset.collapseKind === "gpu") source.gpuCollapsed = !source.gpuCollapsed;
    else source.systemCollapsed = !source.systemCollapsed;
    renderSourceGroups();
  });
}

const dashboardRoot = $("dashboard-sections");
let sectionDragCandidate = null;
function finishDashboardDrag() {
  if (!draggedDashboardSection) return;
  dashboardRoot.classList.remove("section-drag-active");
  dashboardRoot.querySelectorAll(".section-dragging,.section-drop-target").forEach((node) => {
    node.classList.remove("section-dragging", "section-drop-target");
  });
  draggedDashboardSection = null;
  dashboardDropTarget = null;
  renderDashboard();
}
function updateDashboardDropTarget(clientX, clientY) {
  const hit = document.elementFromPoint(clientX, clientY);
  const target = hit?.closest("[data-dashboard-section]");
  if (!target || target.dataset.dashboardSection === draggedDashboardSection) {
    dashboardDropTarget?.classList.remove("section-drop-target");
    dashboardDropTarget = null;
    return;
  }
  if (dashboardDropTarget !== target) {
    dashboardDropTarget?.classList.remove("section-drop-target");
    dashboardDropTarget = target;
    dashboardDropTarget.classList.add("section-drop-target");
  }
}
dashboardRoot.addEventListener("pointerdown", (event) => {
  const handle = event.target.closest("[data-section-drag]");
  if (!handle || event.button !== 0 || !event.isPrimary) return;
  sectionDragCandidate = {
    key: handle.dataset.sectionDrag,
    pointerId: event.pointerId,
    startX: event.clientX,
    startY: event.clientY,
    started: false,
  };
  try { handle.setPointerCapture(event.pointerId); } catch (_) {}
});
document.addEventListener("pointermove", (event) => {
  const candidate = sectionDragCandidate;
  if (!candidate || candidate.pointerId !== event.pointerId) return;
  if (!candidate.started) {
    if (Math.hypot(event.clientX - candidate.startX, event.clientY - candidate.startY) < 6) return;
    const slot = [...dashboardRoot.children].find((child) => child.dataset.dashboardSection === candidate.key);
    if (!slot) { sectionDragCandidate = null; return; }
    draggedDashboardSection = candidate.key;
    candidate.started = true;
    dashboardRoot.classList.add("section-drag-active");
    slot.classList.add("section-dragging");
  }
  event.preventDefault();
  updateDashboardDropTarget(event.clientX, event.clientY);
});
function endSectionPointer(event, commit) {
  const candidate = sectionDragCandidate;
  if (!candidate || candidate.pointerId !== event.pointerId) return;
  if (candidate.started && commit) {
    updateDashboardDropTarget(event.clientX, event.clientY);
    const source = [...dashboardRoot.children].find((child) => child.dataset.dashboardSection === draggedDashboardSection);
    const target = dashboardDropTarget;
    if (source && target && source !== target) {
      const rect = target.getBoundingClientRect();
      if (event.clientY > rect.top + rect.height / 2) target.after(source);
      else target.before(source);
      saveDashboardSectionOrder();
    }
  }
  sectionDragCandidate = null;
  finishDashboardDrag();
}
document.addEventListener("pointerup", (event) => endSectionPointer(event, true));
document.addEventListener("pointercancel", (event) => endSectionPointer(event, false));

$("sign-in-form").addEventListener("submit", (event) => {
  event.preventDefault();
  const token = $("sign-in-token").value.trim();
  if (!token) return;
  try { localStorage.setItem("ctop-token", token); } catch (_) {}
  window.location.href = `/login?token=${encodeURIComponent(token)}`;
});

let savedTheme = null;
try { savedTheme = localStorage.getItem("ctop-theme"); } catch (_) {}
if (savedTheme) document.documentElement.dataset.theme = savedTheme;
$("theme-toggle").addEventListener("click", () => {
  const next = document.documentElement.dataset.theme === "dark" ? "light" : "dark";
  document.documentElement.dataset.theme = next;
  try { localStorage.setItem("ctop-theme", next); } catch (_) {}
});

renderDashboard();
renderMonitorList();
poll();
