"use strict";

const $ = (id) => document.getElementById(id);
const esc = (value) => String(value ?? "").replace(/[&<>"']/g, (ch) => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[ch]));
const validNumber = (value) => value !== null && value !== undefined && value !== "" && Number.isFinite(Number(value));
const number = (value, digits = 1) => validNumber(value) ? Number(value).toLocaleString(undefined, {maximumFractionDigits: digits, minimumFractionDigits: digits}) : "—";
const gib = (bytes, digits = 1) => validNumber(bytes) ? `${number(Number(bytes) / 1073741824, digits)}GB` : "—";
const percent = (value) => validNumber(value) ? `${number(value, 1)}%` : "—";

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

function renderGpuRows(gpus) {
  setText("gpu-count", `${gpus.length} ${gpus.length === 1 ? "device" : "devices"}`);
  const root = $("gpu-rows");
  if (!gpus.length) {
    root.innerHTML = `<tr><td colspan="7" class="empty-row">No GPU devices reported by NVML.</td></tr>`;
    return;
  }
  root.innerHTML = gpus.map((gpu) => {
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
      <td class="mono">${esc(gpu.index)}</td><td>${esc(gpu.name || `GPU ${gpu.index}`)}</td>
      <td class="mono">${gpu.temperature_valid ? `${number(gpu.temperature, 0)}°C` : "—"}</td>
      <td class="mono">${power}</td><td class="mono">${vram}</td>
      <td class="num mono">${gpuPercent === null ? "—" : percent(gpuPercent)}</td>
      <td class="gpu-meter-cell"><div class="gpu-usage-meter" role="img" aria-label="VRAM ${vramPercent === null ? "unavailable" : percent(vramPercent)}, GPU ${gpuPercent === null ? "unavailable" : percent(gpuPercent)}">
        <div class="gpu-meter-half vram-meter"><span style="width:${vramWidth}%"></span></div><div class="gpu-meter-divider"></div>
        <div class="gpu-meter-half gpu-meter"><span style="width:${gpuWidth}%"></span></div></div></td></tr>`;
  }).join("");
}

function renderProcesses(processes) {
  setText("process-count", `${processes.length} ${processes.length === 1 ? "process" : "processes"}`);
  const body = $("process-rows");
  if (!processes.length) {
    body.innerHTML = `<tr><td colspan="9" class="empty-row">No GPU processes reported by NVML.</td></tr>`;
    return;
  }
  body.innerHTML = processes.map((p) => `<tr>
    <td>${esc(p.gpu)}</td><td class="pid">${esc(p.pid)}</td><td class="process-user">${esc(p.user || "?")}</td>
    <td class="num mono">${p.gpu_memory_valid ? gib(p.gpu_memory) : "—"}</td>
    <td class="num mono">${p.sm_valid ? percent(p.sm) : "—"}</td><td class="num mono">${p.cpu_valid ? percent(p.cpu) : "—"}</td>
    <td class="num mono">${p.memory_valid ? percent(p.memory) : "—"}</td><td class="mono">${esc(p.elapsed || "—")}</td>
    <td class="command" title="${esc(p.command)}">${esc(p.command)}</td></tr>`).join("");
}

function render(data) {
  if (validNumber(data.sample_interval) && Number(data.sample_interval) > 0)
    sampleIntervalSeconds = Number(data.sample_interval);
  const ram = data.ram || {}, swap = data.swap || {}, disk = data.disk || {}, net = data.network || {};
  setText("ram-value", percent(ram.percent));
  setText("ram-detail", `(${gib(ram.used)}/${gib(ram.total)})`);
  setText("swap-value", percent(swap.percent));
  setText("swap-detail", `(${gib(swap.used)}/${gib(swap.total)})`);
  const diskTotal = Number(disk.read_bps || 0) + Number(disk.write_bps || 0);
  setText("disk-value", disk.valid ? rate(diskTotal) : "—");
  setText("disk-detail", `(${disk.capacity_valid ? `${gib(disk.used)}/${gib(disk.total)}` : "Capacity unavailable"} · R:${rate(disk.read_bps)} W:${rate(disk.write_bps)})`);
  setText("net-value", net.valid ? networkRate(net.total_bps) : "—");
  setText("net-detail", `(Tot:${net.valid ? networkRate(net.total_bps) : "—"} / ↓${networkRate(net.rx_bps)} ↑${networkRate(net.tx_bps)})`);
  const history = data.history || {};
  drawChart("ram-chart", "ram-axis", history.ram, "percent", 100);
  drawChart("swap-chart", "swap-axis", history.swap, "percent", 100);
  drawChart("disk-chart", "disk-axis", history.disk, "disk");
  drawChart("net-chart", "net-axis", history.network, "network");
  drawChart("vram-chart", "vram-axis", history.vram, "percent", 100);
  drawChart("gpu-chart", "gpu-axis", history.gpu, "percent", 100);
  setText("vram-value", percent(data.averages?.vram_percent));
  setText("gpu-load-value", percent(data.averages?.gpu_percent));
  renderGpuRows(data.gpus || []);
  renderProcesses(data.processes || []);
  setText("updated", `Updated ${new Date().toLocaleTimeString()}`);
  setText("version", `v${esc(data.version || "")}`);
  setConnected();
}

/* A single status indicator lives in the top bar: the LIVE pill carries the
   state, and a short note sits next to it only while the stream is healthy. */
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

async function poll() {
  try {
    const response = await fetch("/api/v1/metrics", {cache: "no-store"});
    if (response.status === 401) { await handleUnauthorized(); return; }
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    render(await response.json());
  } catch (error) {
    setStatus("Connection lost · retrying");
  }
  window.setTimeout(poll, 1000);
}

/* The server hands out an HttpOnly cookie at /login.  The token is also kept in
   localStorage so an expired cookie can be redeemed again without typing it. */
let tokenRedeemed = false;
async function handleUnauthorized() {
  const stored = readStoredToken();
  if (stored && !tokenRedeemed) {
    tokenRedeemed = true;
    try {
      const attempt = await fetch(`/login?token=${encodeURIComponent(stored)}`, {cache: "no-store", redirect: "follow"});
      if (attempt.ok || attempt.status === 302) { window.location.replace("/"); return; }
    } catch (_) {}
  }
  showSignIn();
}

function readStoredToken() {
  try { return localStorage.getItem("ctop-token") || ""; } catch (_) { return ""; }
}

function showSignIn() {
  const overlay = $("sign-in");
  if (!overlay) return;
  overlay.hidden = false;
  setStatus("Sign in required");
  const field = $("sign-in-token");
  field.value = readStoredToken();
  field.focus();
}

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
poll();
