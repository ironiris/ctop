"use strict";

const $ = (id) => document.getElementById(id);
const esc = (value) => String(value ?? "").replace(/[&<>"']/g, (ch) => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[ch]));
const validNumber = (value) => value !== null && value !== undefined && value !== "" && Number.isFinite(Number(value));
const number = (value, digits = 1) => validNumber(value) ? Number(value).toLocaleString(undefined, {maximumFractionDigits: digits, minimumFractionDigits: digits}) : "—";
const gib = (bytes, digits = 1) => validNumber(bytes) ? `${number(Number(bytes) / 1073741824, digits)}GB` : "—";
const percent = (value) => validNumber(value) ? `${number(value, 1)}%` : "—";
const MONITORS_KEY = "ctop-monitors-v1";
const DASHBOARD_ORDER_KEY = "ctop-dashboard-section-order-v1";
function loadDashboardSectionOrder() {
  let stored;
  try { stored = JSON.parse(localStorage.getItem(DASHBOARD_ORDER_KEY) || "[]"); } catch (_) {}
  if (!Array.isArray(stored)) return [];
  return stored.filter((key, index) => typeof key === "string" &&
    (key === "gpu-list" || /^(gpu|system|gpu-processes):[a-zA-Z0-9_-]{1,64}$/.test(key)) &&
    stored.indexOf(key) === index);
}
let dashboardSectionOrder = loadDashboardSectionOrder();
function currentDashboardSectionKeys() {
  return ["gpu-list", ...sources.map((source) => `gpu:${source.id}`),
    ...sources.map((source) => `system:${source.id}`),
    ...sources.map((source) => `gpu-processes:${source.id}`)];
}
function reconcileDashboardSectionOrder() {
  const defaults = currentDashboardSectionKeys();
  dashboardSectionOrder = dashboardSectionOrder.filter((key) => defaults.includes(key));
  for (const key of defaults) {
    if (dashboardSectionOrder.includes(key)) continue;
    const rank = defaults.indexOf(key);
    const insertAt = dashboardSectionOrder.findIndex((current) => defaults.indexOf(current) > rank);
    if (insertAt < 0) dashboardSectionOrder.push(key);
    else dashboardSectionOrder.splice(insertAt, 0, key);
  }
}

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
  const root = $("dashboard-sections");
  for (const key of dashboardSectionOrder) {
    const slot = [...root.children].find((child) => child.dataset.dashboardSection === key);
    if (slot) root.appendChild(slot);
  }
}
function updateDashboardOrderButtons() {
  for (const button of $("dashboard-sections").querySelectorAll("[data-order-section]")) {
    const index = dashboardSectionOrder.indexOf(button.dataset.orderSection);
    const direction = Number(button.dataset.orderDirection);
    button.disabled = direction < 0 ? index <= 0 : index >= dashboardSectionOrder.length - 1;
  }
}
function moveDashboardSection(key, direction) {
  const index = dashboardSectionOrder.indexOf(key);
  const nextIndex = index + direction;
  if (index < 0 || nextIndex < 0 || nextIndex >= dashboardSectionOrder.length) return;
  [dashboardSectionOrder[index], dashboardSectionOrder[nextIndex]] =
    [dashboardSectionOrder[nextIndex], dashboardSectionOrder[index]];
  applyDashboardSectionOrder();
  try { localStorage.setItem(DASHBOARD_ORDER_KEY, JSON.stringify(dashboardSectionOrder)); } catch (_) {}
  updateDashboardOrderButtons();
}
function sectionOrderButtons(key, title) {
  const up = `<button type="button" data-order-section="${key}" data-order-direction="-1" aria-label="Move ${title} up" title="Move up"><svg viewBox="0 0 16 16" aria-hidden="true"><path d="M8 4 13 12H3z"/></svg></button>`;
  const down = `<button type="button" data-order-section="${key}" data-order-direction="1" aria-label="Move ${title} down" title="Move down"><svg viewBox="0 0 16 16" aria-hidden="true"><path d="M3 4h10L8 12z"/></svg></button>`;
  return `<div class="section-order-controls" role="group" aria-label="Reorder ${title}">${up}${down}</div>`;
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
function remoteTupleKey(address, token = "") {
  const url = new URL(address, window.location.href);
  const port = url.port || (url.protocol === "https:" ? "443" : "80");
  return `${url.hostname.toLowerCase()}|${port}|${String(token)}`;
}
function randomSourceId() {
  return `remote-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
}
function loadRemoteSources(localPrefix) {
  let stored;
  try { stored = JSON.parse(localStorage.getItem(MONITORS_KEY) || "[]"); }
  catch (_) { stored = []; }
  if (!Array.isArray(stored)) return [];
  const used = new Set([localPrefix]);
  const localEndpoint = localEndpointKey();
  const tuples = new Set();
  const ids = new Set(["local"]);
  const result = [];
  for (const item of stored) {
    try {
      let url = normalizeMetricsUrl(item?.url);
      const parsedUrl = new URL(url);
      const queryToken = parsedUrl.searchParams.get("token") || "";
      const token = String(item?.token || queryToken);
      if (queryToken) {
        parsedUrl.searchParams.delete("token");
        url = parsedUrl.href;
      }
      const endpoint = endpointKey(url);
      const tuple = remoteTupleKey(url, token);
      if (endpoint === localEndpoint || tuples.has(tuple)) continue;
      let prefix = normalizePrefix(item?.prefix);
      if (!prefix || used.has(prefix)) prefix = nextAvailablePrefix(used);
      if (!prefix) continue;
      const serverId = Number.isSafeInteger(Number(item?.serverId)) && Number(item.serverId) > 0
        ? Number(item.serverId) : 0;
      let id = serverId ? `tui-${serverId}`
        : /^[a-zA-Z0-9_-]{1,64}$/.test(item?.id || "") ? item.id : randomSourceId();
      while (ids.has(id)) id = randomSourceId();
      ids.add(id); used.add(prefix); tuples.add(tuple);
      result.push({id, domId: `source-${id}`, serverId, prefix, url, token, local: false,
        webConfigured: true, serverManaged: serverId > 0, migrationPending: true,
        data: null, error: "", errorCode: 0, gpuCollapsed: false, systemCollapsed: false, processCollapsed: false});
    } catch (_) {}
  }
  return result;
}

const localSource = {id: "local", domId: "source-local", prefix: "A", url: "/api/v1/metrics", local: true, data: null, error: "", errorCode: 0, gpuCollapsed: false, systemCollapsed: false, processCollapsed: false};
let sources = [localSource, ...loadRemoteSources(localSource.prefix)];
let lastMigrationAttempt = 0;
let migrationRunning = false;
let remoteSharingProtected = false;
let renderedSourceLayout = "";
let tokenRedeemed = false;
let pollTimer = null;
let pollRunning = false;
let pollAgain = false;

function monitorData(source, kind) {
  const base = `${source.domId}-${kind}`;
  const sectionKey = `${kind === "process" ? "gpu-processes" : kind}:${source.id}`;
  if (kind === "gpu") {
    const collapsed = source.gpuCollapsed;
    const sectionTitle = sources.length > 1 ? `GPU(${source.prefix})` : "GPU";
    return `<section class="section source-section ${collapsed ? "source-section-collapsed" : ""}" data-dashboard-section="${esc(sectionKey)}" data-monitor-section>
      <div class="section-heading"><div class="section-title-with-order">${sectionOrderButtons(sectionKey, "GPU")}<h2>${esc(sectionTitle)}</h2></div><div class="section-meta source-heading-meta"><span>Average across GPUs</span><button class="collapse-toggle" type="button" data-collapse-source="${esc(source.id)}" data-collapse-kind="gpu" aria-expanded="${!collapsed}" aria-label="${collapsed ? "Expand" : "Collapse"} ${esc(sectionTitle)} graphs" title="${collapsed ? "Expand" : "Collapse"} GPU graphs"><span aria-hidden="true"></span></button></div></div>
      <div class="source-section-body" ${collapsed ? "hidden" : ""}><div class="history-grid">
        <article class="metric-card history-card"><div class="metric-head"><span class="metric-label">Average GPU MEM usage</span></div>
          <div id="${base}-vram-value" class="metric-value">—</div>
          <div class="chart-wrap"><div id="${base}-vram-axis" class="y-axis"></div><svg id="${base}-vram-chart" class="chart" viewBox="0 0 640 84" preserveAspectRatio="none" aria-label="${esc(source.prefix)} GPU MEM history"></svg></div>
          <div class="x-axis"><span>-400s</span><span>-120s</span><span>-30s</span><span>-10s</span><span>now</span></div>
        </article>
        <article class="metric-card history-card"><div class="metric-head"><span class="metric-label">Average GPU utilization</span></div>
          <div id="${base}-gpu-value" class="metric-value">—</div>
          <div class="chart-wrap"><div id="${base}-gpu-axis" class="y-axis"></div><svg id="${base}-gpu-chart" class="chart" viewBox="0 0 640 84" preserveAspectRatio="none" aria-label="${esc(source.prefix)} GPU utilization history"></svg></div>
          <div class="x-axis"><span>-400s</span><span>-120s</span><span>-30s</span><span>-10s</span><span>now</span></div>
        </article>
      </div></div></section>`;
  }
  if (kind === "process") {
    const collapsed = source.processCollapsed;
    const sectionTitle = sources.length > 1 ? `GPU PROCESSES(${source.prefix})` : "GPU PROCESSES";
    return `<section class="section source-section ${collapsed ? "source-section-collapsed" : ""}" data-dashboard-section="${esc(sectionKey)}" data-monitor-section>
      <div class="section-heading"><div class="section-title-with-order">${sectionOrderButtons(sectionKey, "GPU PROCESSES")}<h2>${esc(sectionTitle)}</h2></div><div class="section-meta source-heading-meta"><span id="${source.domId}-process-count">— processes</span><button class="collapse-toggle" type="button" data-collapse-source="${esc(source.id)}" data-collapse-kind="process" aria-expanded="${!collapsed}" aria-label="${collapsed ? "Expand" : "Collapse"} ${esc(sectionTitle)}" title="${collapsed ? "Expand" : "Collapse"} GPU processes"><span aria-hidden="true"></span></button></div></div>
      <div class="source-section-body" ${collapsed ? "hidden" : ""}><div class="table-card"><div class="table-scroll"><table><thead><tr><th>GPU</th><th>PID</th><th>USER</th><th class="num">GPU-MEM</th><th class="num">GPU-LOAD</th><th class="num">CPU-AVG</th><th class="num">MEM</th><th>TIME</th><th>COMMAND</th></tr></thead><tbody id="${source.domId}-process-rows"><tr><td colspan="9" class="empty-row">Waiting for process metrics…</td></tr></tbody></table></div></div></div>
    </section>`;
  }
  const collapsed = source.systemCollapsed;
  const sectionTitle = sources.length > 1 ? `SYSTEM(${source.prefix})` : "SYSTEM";
  const card = (metric, label, icon, detailLabel) => `<article class="metric-card" data-metric="${metric}">
    <div class="metric-head"><span class="metric-label"><i class="metric-icon ${icon}"></i>${label}</span><span id="${base}-${metric}-detail" class="metric-info">—</span></div>
    <div id="${base}-${metric}-value" class="metric-value">—</div>
    <div class="chart-wrap"><div id="${base}-${metric}-axis" class="y-axis"></div><svg id="${base}-${metric}-chart" class="chart" viewBox="0 0 640 84" preserveAspectRatio="none" aria-label="${esc(source.prefix)} ${detailLabel} history"></svg></div>
    <div class="x-axis"><span>-400s</span><span>-120s</span><span>-30s</span><span>-10s</span><span>now</span></div>
  </article>`;
  return `<section class="section source-section ${collapsed ? "source-section-collapsed" : ""}" data-dashboard-section="${esc(sectionKey)}" data-monitor-section>
    <div class="section-heading"><div class="section-title-with-order">${sectionOrderButtons(sectionKey, "SYSTEM")}<h2>${esc(sectionTitle)}</h2></div><div class="section-meta source-heading-meta"><span>400-second history</span><button class="collapse-toggle" type="button" data-collapse-source="${esc(source.id)}" data-collapse-kind="system" aria-expanded="${!collapsed}" aria-label="${collapsed ? "Expand" : "Collapse"} ${esc(sectionTitle)}" title="${collapsed ? "Expand" : "Collapse"} system history"><span aria-hidden="true"></span></button></div></div>
    <div class="source-section-body" ${collapsed ? "hidden" : ""}><div class="metric-grid">
      ${card("ram", "RAM", "ram-icon", "RAM")}
      ${card("swap", "SWAP", "swap-icon", "SWAP")}
      ${card("disk", "DISK", "disk-icon", "disk I/O")}
      ${card("net", "NET", "net-icon", "network")}
    </div></div></section>`;
}

function renderSourceGroups() {
  const root = $("dashboard-sections");
  const layout = JSON.stringify(sources.map((source) => [source.id, source.prefix,
    source.gpuCollapsed, source.systemCollapsed, source.processCollapsed]));
  if (layout !== renderedSourceLayout) {
    root.querySelectorAll("[data-monitor-section]").forEach((section) => section.remove());
    const markup = [
      ...sources.map((source) => monitorData(source, "gpu")),
      ...sources.map((source) => monitorData(source, "system")),
      ...sources.map((source) => monitorData(source, "process")),
    ].join("");
    const fragment = document.createElement("template");
    fragment.innerHTML = markup;
    root.append(fragment.content);
    reconcileDashboardSectionOrder();
    applyDashboardSectionOrder();
    renderedSourceLayout = layout;
  }
  for (const source of sources) {
    if (source.data) renderSourceMetrics(source);
    renderProcessRows(source);
  }
  updateDashboardOrderButtons();
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
      <td class="mono">${sources.length > 1 ? esc(source.prefix) : ""}${esc(gpu.index)}</td><td>${esc(gpu.name || `GPU ${gpu.index}`)}${gpu.source ? ` <span class="gpu-source">${esc(gpu.source)}</span>` : ""}</td>
      <td class="mono">${gpu.temperature_valid ? `${number(gpu.temperature, 0)}°C` : "—"}</td>
      <td class="mono">${power}</td><td class="mono">${vram}</td>
      <td class="num mono">${gpuPercent === null ? "—" : percent(gpuPercent)}</td>
      <td class="gpu-meter-cell"><div class="gpu-usage-meter" role="img" aria-label="GPU MEM ${vramPercent === null ? "unavailable" : percent(vramPercent)}, GPU ${gpuPercent === null ? "unavailable" : percent(gpuPercent)}">
        <div class="gpu-meter-half vram-meter"><span style="width:${vramWidth}%"></span></div><div class="gpu-meter-divider"></div>
        <div class="gpu-meter-half gpu-meter"><span style="width:${gpuWidth}%"></span></div></div></td></tr>`;
  }).join("");
}

function renderProcessRows(source) {
  const processes = source.data?.processes || [];
  const countId = `${source.domId}-process-count`;
  const body = $(`${source.domId}-process-rows`);
  if (!body) return;
  setText(countId, `${processes.length} ${processes.length === 1 ? "process" : "processes"}`);
  if (!processes.length) {
    body.innerHTML = `<tr><td colspan="9" class="empty-row">${source.data ? "No GPU processes reported by this monitor." : "Waiting for process metrics…"}</td></tr>`;
    return;
  }
  body.innerHTML = processes.map((p) => `<tr>
    <td class="mono">${sources.length > 1 ? esc(source.prefix) : ""}${esc(p.gpu)}</td><td class="pid">${esc(p.pid)}</td><td class="process-user">${esc(p.user || "?")}</td>
    <td class="num mono">${p.gpu_memory_valid ? gib(p.gpu_memory) : "—"}</td>
    <td class="num mono">${p.sm_valid ? percent(p.sm) : "—"}</td><td class="num mono">${p.cpu_valid ? percent(p.cpu) : "—"}</td>
    <td class="num mono">${p.memory_valid ? percent(p.memory) : "—"}</td><td class="mono">${esc(p.elapsed || "—")}</td>
    <td class="command" title="${esc(p.command)}">${esc(p.command)}</td></tr>`).join("");
}

function saveSources() {
  try {
    localStorage.setItem(MONITORS_KEY, JSON.stringify(sources.filter((source) => source.webConfigured)
      .map(({id, serverId, prefix, url, token}) => ({id, serverId, prefix, url, token}))));
  } catch (_) {}
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
function syncServerMonitors(data) {
  const previous = sources.filter((source) => !source.local);
  const matched = new Set();
  const managed = [];
  const monitors = Array.isArray(data?.monitors) ? data.monitors : [];
  remoteSharingProtected = Boolean(data?.monitors_protected);
  for (const monitor of monitors) {
    const serverId = Number(monitor?.id);
    if (!Number.isSafeInteger(serverId) || serverId < 1) continue;
    let source = previous.find((item) => item.serverId === serverId);
    if (!source) source = {local: false, webConfigured: false, token: "", gpuCollapsed: false,
      systemCollapsed: false, processCollapsed: false};
    source.serverId = serverId;
    source.id = `tui-${serverId}`;
    source.domId = `source-${source.id}`;
    source.prefix = normalizePrefix(monitor.prefix) || source.prefix || "B";
    source.url = String(monitor.address || source.url || "");
    source.serverManaged = true;
    source.migrationPending = false;
    source.data = monitor.metrics && typeof monitor.metrics === "object" ? monitor.metrics : null;
    source.error = monitor.connected ? "" : String(monitor.status || "Connection lost");
    source.errorCode = 0;
    matched.add(serverId);
    managed.push(source);
  }
  const pending = previous.filter((source) => source.webConfigured &&
    (source.migrationPending || (remoteSharingProtected && !matched.has(source.serverId))));
  for (const source of pending) {
    source.serverManaged = true;
    if (remoteSharingProtected && !source.migrationPending)
      source.error = "Shared monitors require --token or --localhost";
    managed.push(source);
  }
  sources = [localSource, ...managed];
  localSource.prefix = monitors.length ? "A" : "";
  saveSources();
}
async function migrateSavedMonitors() {
  if (migrationRunning || Date.now() - lastMigrationAttempt < 5000) return;
  const pending = sources.filter((source) => source.webConfigured && source.migrationPending);
  if (!pending.length) return;
  migrationRunning = true;
  lastMigrationAttempt = Date.now();
  try {
    for (const source of pending) {
      try {
        const response = await fetch("/api/v1/monitors", {
          method: "POST", headers: {"Content-Type": "application/json"},
          body: JSON.stringify({address: source.url, token: source.token})
        });
        if (!response.ok) throw new Error((await response.text()).trim() || `HTTP ${response.status}`);
        const result = await response.json();
        source.serverId = Number(result.id);
        source.id = `tui-${source.serverId}`;
        source.domId = `source-${source.id}`;
        source.prefix = normalizePrefix(result.prefix) || source.prefix;
        source.url = result.address || source.url;
        source.serverManaged = true;
        source.migrationPending = false;
        source.error = "";
        saveSources();
      } catch (error) {
        source.serverManaged = true;
        source.error = String(error?.message || "Could not add shared monitor").replace(/^Failed to fetch$/, "Connection failed");
      }
    }
  } finally {
    migrationRunning = false;
  }
}
function renderMonitorList() {
  const root = $("monitor-list");
  const remotes = sources.filter((source) => !source.local);
  if (!remotes.length) {
    root.innerHTML = remoteSharingProtected
      ? `<li class="monitor-empty">TUI monitors require --token or --localhost to share with the Web UI.</li>`
      : `<li class="monitor-empty">No additional monitors.</li>`;
    return;
  }
  root.innerHTML = remotes.map((source) => `<li class="monitor-item">
    <span class="monitor-prefix">Prefix ${esc(source.prefix)}</span>
    <span class="monitor-address" title="${esc(displayMetricsAddress(source.url))}">${esc(displayMetricsAddress(source.url))}</span>
    <span class="monitor-state ${source.error ? "offline" : ""}">${source.error ? esc(source.error) : source.data ? "Connected" : "Connecting"}</span>
    <button type="button" class="monitor-remove" data-remove-source="${esc(source.id)}" aria-label="Remove monitor ${esc(source.prefix)}" title="Remove monitor">×</button>
  </li>`).join("");
}
function renderDashboard() {
  renderGpuRows();
  renderSourceGroups();
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
    const priorErrors = new Map(sources.map((source) => [source.id, Boolean(source.error)]));
    const localWasDisconnected = Boolean(localSource.error);
    await migrateSavedMonitors();
    let localUnauthorized = false;
    try {
      const controller = new AbortController();
      const timeout = window.setTimeout(() => controller.abort(), 5000);
      let response;
      try { response = await fetch(localSource.url, {signal: controller.signal, cache: "no-store"}); }
      finally { window.clearTimeout(timeout); }
      if (response.status === 401) localUnauthorized = true;
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const data = await response.json();
      if (!data || typeof data !== "object" || !Array.isArray(data.gpus) || !data.history)
        throw new Error("Invalid metrics response");
      localSource.data = data;
      localSource.error = "";
      localSource.errorCode = 0;
      syncServerMonitors(data);
    } catch (error) {
      localSource.error = String(error?.message || "Connection failed").replace(/^Failed to fetch$/, "Connection failed");
      const match = localSource.error.match(/HTTP (\d+)/);
      localSource.errorCode = match ? Number(match[1]) : 0;
    }
    renderDashboard();
    const recovered = (localWasDisconnected && !localSource.error) ||
      sources.some((source) => priorErrors.get(source.id) && !source.error);
    if (recovered) {
      window.location.reload();
      return;
    }
    const failed = sources.filter((source) => source.error).length;
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

$("monitor-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  let url;
  try { url = normalizeMetricsUrl($("monitor-url").value); }
  catch (error) { setFormMessage(error.message, true); return; }
  let token = $("monitor-token").value.trim();
  const parsedUrl = new URL(url);
  const queryToken = parsedUrl.searchParams.get("token") || "";
  if (token && queryToken && token !== queryToken) {
    setFormMessage("Use either the Token field or token query parameter, not both.", true);
    return;
  }
  if (!token && queryToken) token = queryToken;
  if (queryToken) {
    parsedUrl.searchParams.delete("token");
    url = parsedUrl.href;
  }
  if (token && !/^[A-Za-z0-9._~-]{8,128}$/.test(token)) {
    setFormMessage("Token must be 8–128 characters using A–Z, a–z, 0–9, ., _, -, or ~.", true);
    return;
  }
  const button = $("monitor-form").querySelector("button[type=submit]");
  button.disabled = true;
  try {
    const response = await fetch("/api/v1/monitors", {
      method: "POST", headers: {"Content-Type": "application/json"},
      body: JSON.stringify({address: url, token})
    });
    if (response.status === 401) { await handleUnauthorized(); return; }
    if (!response.ok) throw new Error((await response.text()).trim() || `HTTP ${response.status}`);
    const result = await response.json();
    const serverId = Number(result.id);
    let source = sources.find((item) => item.serverId === serverId);
    if (!source) {
      source = {id: `tui-${serverId}`, domId: `source-tui-${serverId}`, serverId,
        prefix: result.prefix, url: result.address || url, token, local: false,
        webConfigured: true, serverManaged: true, migrationPending: false,
        data: null, error: "", errorCode: 0, gpuCollapsed: false,
        systemCollapsed: false, processCollapsed: false};
      sources.push(source);
    }
    source.webConfigured = true;
    source.serverManaged = true;
    source.migrationPending = false;
    source.token = token;
    source.prefix = normalizePrefix(result.prefix) || source.prefix;
    source.url = result.address || url;
    saveSources();
    $("monitor-url").value = "";
    $("monitor-token").value = "";
    setFormMessage(result.duplicate
      ? `Monitor ${source.prefix} is already configured in the TUI.`
      : `Monitor ${source.prefix} added to the shared TUI/Web monitor list.`);
    renderDashboard();
    if (pollTimer) clearTimeout(pollTimer);
    poll();
  } catch (error) {
    setFormMessage(error.message || "Could not add monitor.", true);
  } finally {
    button.disabled = false;
  }
});

$("monitor-list").addEventListener("click", async (event) => {
  const button = event.target.closest("[data-remove-source]");
  if (!button) return;
  const source = sources.find((item) => item.id === button.dataset.removeSource);
  if (!source) return;
  button.disabled = true;
  try {
    if (source.serverId) {
      const response = await fetch(`/api/v1/monitors/${source.serverId}`, {method: "DELETE"});
      if (response.status === 401) { await handleUnauthorized(); return; }
      if (!response.ok) throw new Error((await response.text()).trim() || `HTTP ${response.status}`);
    }
    sources = sources.filter((item) => item !== source);
    saveSources();
    renderDashboard();
    setFormMessage("");
    if (pollTimer) clearTimeout(pollTimer);
    poll();
  } catch (error) {
    setFormMessage(error.message || "Could not remove monitor.", true);
    button.disabled = false;
  }
});

const dashboardRoot = $("dashboard-sections");
dashboardRoot.addEventListener("click", (event) => {
  const collapseButton = event.target.closest("[data-collapse-source]");
  if (collapseButton) {
    const source = sources.find((item) => item.id === collapseButton.dataset.collapseSource);
    if (!source) return;
    if (collapseButton.dataset.collapseKind === "gpu") source.gpuCollapsed = !source.gpuCollapsed;
    else if (collapseButton.dataset.collapseKind === "system") source.systemCollapsed = !source.systemCollapsed;
    else source.processCollapsed = !source.processCollapsed;
    renderSourceGroups();
    return;
  }
  const orderButton = event.target.closest("[data-order-section]");
  if (orderButton) moveDashboardSection(orderButton.dataset.orderSection, Number(orderButton.dataset.orderDirection));
});

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

let captureToastTimer = 0;
function showCaptureToast(message, isError = false) {
  const toast = $("capture-status");
  toast.textContent = message;
  toast.classList.toggle("error", isError);
  toast.hidden = false;
  clearTimeout(captureToastTimer);
  captureToastTimer = setTimeout(() => { toast.hidden = true; }, 3500);
}

function dashboardScreenshotBlob() {
  const topbar = document.querySelector(".topbar");
  const footer = document.querySelector("footer");
  if (!topbar || !footer || typeof window.html2canvas !== "function") {
    return Promise.reject(new Error("Dashboard capture is unavailable"));
  }

  const topbarRect = topbar.getBoundingClientRect();
  const footerRect = footer.getBoundingClientRect();
  const sidePadding = 10;
  const left = Math.max(0, Math.floor(topbarRect.left + window.scrollX - sidePadding));
  const right = Math.min(document.documentElement.scrollWidth,
                         Math.ceil(topbarRect.right + window.scrollX + sidePadding));
  const bottom = Math.floor(footerRect.top + window.scrollY);
  const width = right - left;
  if (width <= 0 || bottom <= 0) return Promise.reject(new Error("Invalid dashboard bounds"));

  const scale = Math.min(window.devicePixelRatio || 1, 2);
  return window.html2canvas(document.body, {
    windowWidth: document.documentElement.clientWidth,
    windowHeight: window.innerHeight,
    scrollX: 0,
    scrollY: 0,
    scale,
    foreignObjectRendering: true,
    backgroundColor: getComputedStyle(document.body).backgroundColor,
    useCORS: true,
    logging: false,
    onclone: (documentClone) => documentClone.getElementById("capture-button")?.removeAttribute("disabled"),
    ignoreElements: (element) => element.tagName === "FOOTER" || element.id === "capture-status",
  }).then((source) => new Promise((resolve, reject) => {
    const sourceScale = source.width / document.documentElement.clientWidth;
    const crop = document.createElement("canvas");
    crop.width = Math.round(width * sourceScale);
    crop.height = Math.min(Math.round(bottom * sourceScale), source.height);
    const context = crop.getContext("2d");
    if (!context || crop.width <= 0 || crop.height <= 0) {
      reject(new Error("Screenshot crop failed"));
      return;
    }
    context.drawImage(source, Math.round(left * sourceScale), 0, crop.width, crop.height,
                      0, 0, crop.width, crop.height);
    crop.toBlob((blob) => blob ? resolve(blob) : reject(new Error("PNG encoding failed")), "image/png");
  }));
}

function downloadScreenshot(blob) {
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  link.href = url;
  link.download = `ctop-dashboard-${new Date().toISOString().replace(/[:.]/g, "-")}.png`;
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 5000);
}

$("capture-button").addEventListener("click", async (event) => {
  const button = event.currentTarget;
  if (button.disabled) return;
  button.disabled = true;

  let imagePromise = null;
  try {
    if (!window.isSecureContext || !navigator.clipboard?.write || typeof ClipboardItem === "undefined") {
      downloadScreenshot(await dashboardScreenshotBlob());
      showCaptureToast("Clipboard unavailable; image downloaded", true);
      return;
    }

    imagePromise = dashboardScreenshotBlob();
    await navigator.clipboard.write([new ClipboardItem({ "image/png": imagePromise })]);
    showCaptureToast("Dashboard copied to clipboard");
  } catch (error) {
    console.error("Dashboard capture failed:", error?.message || String(error), error?.stack || "");
    if (imagePromise) {
      try {
        downloadScreenshot(await imagePromise);
        showCaptureToast("Clipboard failed; image downloaded", true);
      } catch (_) {
        showCaptureToast("Could not copy or download dashboard", true);
      }
    } else {
      showCaptureToast("Could not capture dashboard", true);
    }
  } finally {
    button.disabled = false;
  }
});

poll();
