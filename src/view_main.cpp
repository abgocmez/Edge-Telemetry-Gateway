// Live view: a consumer that serves what it sees over HTTP.
//
// It observes the gateway the same way every other consumer does - by
// connecting to a port and decoding the documented protocol. Nothing here
// reaches into the gateway, and there is no debug hook or side channel. That is
// the point: if the fan-out architecture is real, watching it should require
// being a consumer, not being privileged.
//
// Consequences worth stating. This view sees only what a consumer can see:
// frames, sequence gaps, and its own latency. It cannot see queue depth,
// would_block, or per-consumer drop counts, because those live in the gateway
// and no consumer is told about them - the gateway prints them itself.
//
// Two threads: one draining the stream, one serving HTTP, sharing state under a
// mutex. The lock is held only to copy small aggregates, never across a socket
// operation, so a stalled browser cannot slow down frame consumption.

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "frame_stream.hpp"
#include "gap_tracker.hpp"
#include "http.hpp"
#include "samples.hpp"
#include "source.hpp"
#include "version.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

constexpr std::size_t kRateWindow = 60;   // seconds of history kept
constexpr std::size_t kRecentFrames = 40;
constexpr std::size_t kMaxIds = 256;      // bound the id map against a fuzzed bus

// Latency is only a delay while the gateway shares this machine's clock. The
// test for that, and why it is needed, is etg::same_clock_domain in source.hpp.

// A live view shows current behaviour, so latency here is a window over the
// recent past rather than a lifetime aggregate.
//
// The window is defined by *time*, not by a sample count. A fixed count sounds
// equivalent but is not: 32k samples is eleven seconds at 3k frames/s and a
// third of a second at 100k/s, so the panel would silently mean something
// different at every load level. A count cap still exists, but only to bound
// memory - at which point the window is shorter than requested and still
// recent, which is the right way to fail.
//
// A cumulative figure would instead be permanently poisoned by the backlog this
// consumer drained when it first connected - the same effect that made the
// probe report p99 = 345ms before --warmup existed - and unlike a measurement
// run, a dashboard cannot be restarted to clear it.
//
// For an authoritative number, use etg-probe with --warmup. This panel is for
// watching, not for publishing.
class RollingLatency {
 public:
  static constexpr std::uint64_t kWindowNs = 10'000'000'000ULL;  // 10 s
  static constexpr std::size_t kMaxSamples = 65536;              // memory bound

  void add(std::uint64_t now, std::int64_t v) {
    samples_.push_back({now, v});
    const std::uint64_t cutoff = now > kWindowNs ? now - kWindowNs : 0;
    while (!samples_.empty() &&
           (samples_.front().t < cutoff || samples_.size() > kMaxSamples)) {
      samples_.pop_front();
    }
  }

  [[nodiscard]] etg::Percentiles compute() const {
    etg::Percentiles p;
    if (samples_.empty()) {
      return p;
    }
    std::vector<std::int64_t> v;
    v.reserve(samples_.size());
    for (const Sample& s : samples_) {
      v.push_back(s.v);
    }
    std::sort(v.begin(), v.end());

    const auto at = [&v](double q) {
      auto i = static_cast<std::size_t>(q * static_cast<double>(v.size()));
      return v[i >= v.size() ? v.size() - 1 : i];
    };
    p.count = v.size();
    p.min = v.front();
    p.max = v.back();
    p.p50 = at(0.50);
    p.p90 = at(0.90);
    p.p99 = at(0.99);
    p.p999 = at(0.999);
    return p;
  }

  // How much wall time the retained samples actually span, so the page can say
  // when the count cap has shortened the window rather than implying 10 s.
  [[nodiscard]] double span_s() const {
    if (samples_.size() < 2) {
      return 0.0;
    }
    return static_cast<double>(samples_.back().t - samples_.front().t) / 1e9;
  }

 private:
  struct Sample {
    std::uint64_t t;
    std::int64_t v;
  };
  std::deque<Sample> samples_;
};

struct IdStat {
  std::uint64_t count = 0;
  std::uint64_t last_seen_ns = 0;
  std::uint8_t src_id = 0;
  std::uint8_t len = 0;
  std::array<std::uint8_t, 8> data{};
};

struct State {
  std::mutex mutex;

  bool connected = false;
  std::uint64_t frames = 0;
  std::uint64_t batches = 0;
  std::uint64_t reconnects = 0;
  std::uint64_t protocol_errs = 0;
  std::uint64_t first_seq = 0;
  std::uint64_t last_seq = 0;
  bool have_seq = false;
  etg::GapTracker gaps;

  std::map<std::uint32_t, IdStat> ids;
  std::deque<etg::Frame> recent;
  std::vector<std::uint64_t> rate{std::vector<std::uint64_t>(kRateWindow, 0)};
  std::size_t rate_pos = 0;
  std::uint64_t frames_at_tick = 0;

  RollingLatency latency;
  std::uint64_t off_domain = 0;  // samples rejected as not from this clock
  std::uint64_t started_ns = 0;
};

std::string hex_payload(const etg::Frame& f) {
  static const char* kHex = "0123456789ABCDEF";
  std::string s;
  s.reserve(static_cast<std::size_t>(f.len) * 2);
  for (std::uint8_t i = 0; i < f.len; ++i) {
    s.push_back(kHex[f.data[i] >> 4U]);
    s.push_back(kHex[f.data[i] & 0x0FU]);
  }
  return s;
}

std::string json_stats(State& st, std::uint16_t port) {
  std::string out;
  out.reserve(16384);

  std::lock_guard<std::mutex> lock(st.mutex);
  const etg::Percentiles p = st.latency.compute();
  const std::uint64_t now = etg::monotonic_ns();
  const double uptime = static_cast<double>(now - st.started_ns) / 1e9;

  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "{\"connected\":%s,\"port\":%u,\"uptime_s\":%.1f,"
                "\"frames\":%llu,\"batches\":%llu,\"markers\":%llu,\"missing\":%llu,\"silent\":%llu,"
                "\"reconnects\":%llu,\"protocol_errs\":%llu,"
                "\"first_seq\":%llu,\"last_seq\":%llu,",
                st.connected ? "true" : "false", port, uptime,
                static_cast<unsigned long long>(st.frames),
                static_cast<unsigned long long>(st.batches),
                static_cast<unsigned long long>(st.gaps.stats().markers),
                static_cast<unsigned long long>(st.gaps.total_lost()),
                static_cast<unsigned long long>(st.gaps.stats().silent_jumps),
                static_cast<unsigned long long>(st.reconnects),
                static_cast<unsigned long long>(st.protocol_errs),
                static_cast<unsigned long long>(st.first_seq),
                static_cast<unsigned long long>(st.last_seq));
  out += buf;

  std::snprintf(buf, sizeof(buf),
                "\"latency\":{\"n\":%llu,\"off_domain\":%llu,\"shared_clock\":%s,"
                "\"span_s\":%.1f,\"min\":%lld,\"p50\":%lld,\"p90\":%lld,"
                "\"p99\":%lld,\"p999\":%lld,\"max\":%lld},",
                static_cast<unsigned long long>(p.count),
                static_cast<unsigned long long>(st.off_domain),
                st.off_domain > st.frames / 2 ? "false" : "true", st.latency.span_s(),
                static_cast<long long>(p.min),
                static_cast<long long>(p.p50), static_cast<long long>(p.p90),
                static_cast<long long>(p.p99), static_cast<long long>(p.p999),
                static_cast<long long>(p.max));
  out += buf;

  // Oldest first, so the chart reads left to right without the client having to
  // know where the ring's write cursor is.
  out += "\"rate\":[";
  for (std::size_t i = 0; i < kRateWindow; ++i) {
    const std::size_t idx = (st.rate_pos + i) % kRateWindow;
    if (i != 0) {
      out += ',';
    }
    out += std::to_string(st.rate[idx]);
  }
  out += "],\"ids\":[";

  bool first = true;
  for (const auto& [id, s] : st.ids) {
    if (!first) {
      out += ',';
    }
    first = false;
    std::snprintf(buf, sizeof(buf),
                  "{\"id\":%u,\"count\":%llu,\"src\":%u,\"len\":%u,\"age_ms\":%.0f,\"data\":\"",
                  id, static_cast<unsigned long long>(s.count),
                  static_cast<unsigned>(s.src_id), static_cast<unsigned>(s.len),
                  static_cast<double>(now - s.last_seen_ns) / 1e6);
    out += buf;
    static const char* kHex = "0123456789ABCDEF";
    for (std::uint8_t i = 0; i < s.len; ++i) {
      out.push_back(kHex[s.data[i] >> 4U]);
      out.push_back(kHex[s.data[i] & 0x0FU]);
    }
    out += "\"}";
  }

  out += "],\"recent\":[";
  first = true;
  for (auto it = st.recent.rbegin(); it != st.recent.rend(); ++it) {
    if (!first) {
      out += ',';
    }
    first = false;
    std::snprintf(buf, sizeof(buf),
                  "{\"seq\":%llu,\"id\":%u,\"src\":%u,\"len\":%u,\"data\":\"%s\"}",
                  static_cast<unsigned long long>(it->seq), it->can_id,
                  static_cast<unsigned>(it->src_id), static_cast<unsigned>(it->len),
                  hex_payload(*it).c_str());
    out += buf;
  }
  out += "]}";
  return out;
}

// Served from the binary with no external requests: an edge gateway should not
// need the internet to show its own state.
const char* const kPage = R"PAGE(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Edge Telemetry Gateway — live</title>
<style>
:root{
  color-scheme: light;
  --plane:#f9f9f7; --surface:#fcfcfb;
  --ink:#0b0b0b; --ink-2:#52514e; --muted:#898781;
  --grid:#e1e0d9; --axis:#c3c2b7; --ring:rgba(11,11,11,.10);
  --series:#2a78d6; --series-soft:rgba(42,120,214,.14);
  --good:#0ca30c; --critical:#d03b3b;
}
@media (prefers-color-scheme: dark){
  :root:not([data-theme="light"]){
    color-scheme: dark;
    --plane:#0d0d0d; --surface:#1a1a19;
    --ink:#fff; --ink-2:#c3c2b7; --muted:#898781;
    --grid:#2c2c2a; --axis:#383835; --ring:rgba(255,255,255,.10);
    --series:#3987e5; --series-soft:rgba(57,135,229,.18);
  }
}
:root[data-theme="dark"]{
  color-scheme: dark;
  --plane:#0d0d0d; --surface:#1a1a19;
  --ink:#fff; --ink-2:#c3c2b7; --muted:#898781;
  --grid:#2c2c2a; --axis:#383835; --ring:rgba(255,255,255,.10);
  --series:#3987e5; --series-soft:rgba(57,135,229,.18);
}
*{box-sizing:border-box}
body{margin:0;background:var(--plane);color:var(--ink);
  font:14px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif}
.wrap{max-width:1180px;margin:0 auto;padding:24px 20px 64px}
header{display:flex;align-items:baseline;gap:14px;flex-wrap:wrap;margin-bottom:22px}
h1{font-size:19px;font-weight:650;margin:0;letter-spacing:-.01em}
.sub{color:var(--muted);font-size:13px}
.pill{display:inline-flex;align-items:center;gap:6px;font-size:12px;font-weight:600;
  padding:3px 10px;border-radius:999px;border:1px solid var(--ring)}
.dot{width:7px;height:7px;border-radius:50%;background:currentColor}
.up{color:var(--good)} .down{color:var(--critical)}
.card{background:var(--surface);border:1px solid var(--ring);border-radius:10px;padding:16px 18px}
.grid{display:grid;gap:12px}
.kpis{grid-template-columns:repeat(auto-fit,minmax(150px,1fr));margin-bottom:12px}
.kpi .label{color:var(--muted);font-size:12px;text-transform:uppercase;letter-spacing:.05em}
.kpi .value{font-size:27px;font-weight:640;letter-spacing:-.02em;margin-top:3px}
.kpi .unit{font-size:14px;font-weight:450;color:var(--ink-2);margin-left:3px}
h2{font-size:13px;font-weight:640;margin:0 0 12px;color:var(--ink-2)}
.pcts{display:grid;grid-template-columns:repeat(auto-fit,minmax(96px,1fr));gap:10px}
.pct .k{color:var(--muted);font-size:11px;font-variant-numeric:tabular-nums}
.pct .v{font-size:17px;font-weight:600;font-variant-numeric:tabular-nums;margin-top:1px}
.two{grid-template-columns:1fr 1fr;align-items:start}
@media(max-width:860px){.two{grid-template-columns:1fr}}
table{width:100%;border-collapse:collapse;font-variant-numeric:tabular-nums;font-size:13px}
th{text-align:left;font-weight:600;color:var(--muted);font-size:11px;
  text-transform:uppercase;letter-spacing:.05em;padding:0 8px 7px 0;border-bottom:1px solid var(--grid)}
td{padding:6px 8px 6px 0;border-bottom:1px solid var(--grid);white-space:nowrap}
tr:last-child td{border-bottom:none}
.mono{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px;color:var(--ink-2)}
.num{text-align:right}
.bar{height:6px;border-radius:3px;background:var(--series);min-width:2px;display:block}
.barcell{width:120px}
.scroll{max-height:340px;overflow-y:auto;overflow-x:auto}
.empty{color:var(--muted);padding:20px 0;text-align:center}
#tip{position:fixed;pointer-events:none;opacity:0;transition:opacity .1s;
  background:var(--surface);border:1px solid var(--ring);border-radius:7px;
  padding:7px 10px;font-size:12px;box-shadow:0 3px 12px rgba(0,0,0,.14);z-index:9}
#tip .t{color:var(--muted);font-size:11px}
#tip .b{font-weight:640;font-variant-numeric:tabular-nums}
footer{margin-top:22px;color:var(--muted);font-size:12px}
.note{margin:0;color:var(--ink-2);font-size:13px;max-width:62ch}
</style></head><body>
<div class="wrap">
  <header>
    <h1>Edge Telemetry Gateway</h1>
    <span id="conn" class="pill down"><span class="dot"></span><span id="connText">connecting</span></span>
    <span class="sub" id="meta"></span>
  </header>

  <div class="grid kpis">
    <div class="card kpi"><div class="label">Rate</div>
      <div class="value"><span id="k-rate">–</span><span class="unit">frames/s</span></div></div>
    <div class="card kpi"><div class="label">Frames</div>
      <div class="value" id="k-frames">–</div></div>
    <div class="card kpi"><div class="label">Loss markers</div>
      <div class="value" id="k-gaps">–</div></div>
    <div class="card kpi"><div class="label">Missing</div>
      <div class="value" id="k-missing">–</div></div>
    <div class="card kpi"><div class="label">CAN ids</div>
      <div class="value" id="k-ids">–</div></div>
  </div>

  <div class="card" style="margin-bottom:12px">
    <h2>Frames per second, last 60 s</h2>
    <svg id="chart" width="100%" height="150" role="img"
         aria-label="Frames received per second over the last 60 seconds"></svg>
  </div>

  <div class="card" style="margin-bottom:12px">
    <h2>Consumer latency — ingest to here <span id="l-window" class="sub"></span></h2>
    <p id="l-note" class="note" hidden>Not measurable from here. The gateway is on
      another machine, so its ingest timestamps and this clock count from different
      boots and the difference between them is not a delay. Round-trip probes
      measure the cross-machine path instead; see <span class="mono">--echo-ms</span>.</p>
    <div class="pcts" id="l-pcts">
      <div class="pct"><div class="k">min</div><div class="v" id="l-min">–</div></div>
      <div class="pct"><div class="k">p50</div><div class="v" id="l-p50">–</div></div>
      <div class="pct"><div class="k">p90</div><div class="v" id="l-p90">–</div></div>
      <div class="pct"><div class="k">p99</div><div class="v" id="l-p99">–</div></div>
      <div class="pct"><div class="k">p99.9</div><div class="v" id="l-p999">–</div></div>
      <div class="pct"><div class="k">max</div><div class="v" id="l-max">–</div></div>
    </div>
  </div>

  <div class="grid two">
    <div class="card">
      <h2>Traffic by CAN id</h2>
      <div class="scroll"><table>
        <thead><tr><th>id</th><th>src</th><th class="num">frames</th>
          <th class="barcell"></th><th>payload</th><th class="num">age</th></tr></thead>
        <tbody id="ids"><tr><td colspan="6" class="empty">waiting for frames</td></tr></tbody>
      </table></div>
    </div>
    <div class="card">
      <h2>Most recent frames</h2>
      <div class="scroll"><table>
        <thead><tr><th class="num">seq</th><th>id</th><th>src</th><th>payload</th></tr></thead>
        <tbody id="recent"><tr><td colspan="4" class="empty">waiting for frames</td></tr></tbody>
      </table></div>
    </div>
  </div>

  <footer>
    Observed through the same protocol as any other consumer. Queue depth, drop counts and
    back-pressure live in the gateway and are printed there — a consumer is not told about them.
    Latency is a rolling window for watching, not a measurement: for numbers to publish,
    run <span class="mono">etg-probe --warmup</span>.
  </footer>
</div>
<div id="tip"></div>

<script>
const $ = id => document.getElementById(id);
const nf = new Intl.NumberFormat();
let rate = [];

function ns(v){
  if (v === 0) return "0";
  if (Math.abs(v) < 1000) return v + " ns";
  if (Math.abs(v) < 1e6) return (v/1e3).toFixed(1) + " µs";
  return (v/1e6).toFixed(2) + " ms";
}
function age(ms){
  if (ms < 1000) return Math.round(ms) + " ms";
  if (ms < 60000) return (ms/1000).toFixed(1) + " s";
  return Math.round(ms/60000) + " m";
}

function draw(){
  const svg = $("chart"), W = svg.clientWidth || 900, H = 150;
  const padL = 46, padR = 8, padT = 10, padB = 20;
  const w = W - padL - padR, h = H - padT - padB;
  const peak = Math.max(1, ...rate);
  // Round the ceiling up to something readable so the axis label is not 4173.
  const mag = Math.pow(10, Math.floor(Math.log10(peak)));
  const top = Math.ceil(peak / mag) * mag;
  const x = i => padL + (rate.length < 2 ? 0 : (i / (rate.length - 1)) * w);
  const y = v => padT + h - (v / top) * h;

  let grid = "", ticks = "";
  for (let g = 0; g <= 2; g++){
    const v = (top / 2) * g, yy = y(v);
    grid += `<line x1="${padL}" x2="${W-padR}" y1="${yy}" y2="${yy}" stroke="var(--grid)" stroke-width="1"/>`;
    ticks += `<text x="${padL-8}" y="${yy+4}" text-anchor="end" fill="var(--muted)" font-size="11">${nf.format(Math.round(v))}</text>`;
  }
  const line = rate.map((v,i) => `${i?"L":"M"}${x(i).toFixed(1)},${y(v).toFixed(1)}`).join("");
  const area = line + `L${x(rate.length-1).toFixed(1)},${padT+h}L${padL},${padT+h}Z`;

  svg.setAttribute("viewBox", `0 0 ${W} ${H}`);
  svg.innerHTML = grid +
    `<path d="${area}" fill="var(--series-soft)"/>` +
    `<path d="${line}" fill="none" stroke="var(--series)" stroke-width="2"
       stroke-linejoin="round" stroke-linecap="round"/>` +
    `<line x1="${padL}" x2="${W-padR}" y1="${padT+h}" y2="${padT+h}" stroke="var(--axis)"/>` +
    ticks +
    `<text x="${padL}" y="${H-4}" fill="var(--muted)" font-size="11">60 s ago</text>` +
    `<text x="${W-padR}" y="${H-4}" text-anchor="end" fill="var(--muted)" font-size="11">now</text>` +
    `<line id="cross" x1="0" x2="0" y1="${padT}" y2="${padT+h}" stroke="var(--axis)" stroke-width="1" opacity="0"/>` +
    `<circle id="dot" r="4" fill="var(--series)" stroke="var(--surface)" stroke-width="2" opacity="0"/>`;

  svg.onmousemove = e => {
    const r = svg.getBoundingClientRect();
    const px = (e.clientX - r.left) * (W / r.width);
    let i = Math.round(((px - padL) / w) * (rate.length - 1));
    i = Math.max(0, Math.min(rate.length - 1, i));
    const cx = x(i), cy = y(rate[i]);
    const cross = svg.querySelector("#cross"), dot = svg.querySelector("#dot");
    cross.setAttribute("x1", cx); cross.setAttribute("x2", cx); cross.setAttribute("opacity", "1");
    dot.setAttribute("cx", cx); dot.setAttribute("cy", cy); dot.setAttribute("opacity", "1");
    const tip = $("tip");
    tip.innerHTML = `<div class="t">${rate.length-1-i} s ago</div>` +
                    `<div class="b">${nf.format(rate[i])} frames/s</div>`;
    tip.style.opacity = "1";
    tip.style.left = Math.min(e.clientX + 14, window.innerWidth - 140) + "px";
    tip.style.top = (e.clientY - 46) + "px";
  };
  svg.onmouseleave = () => {
    $("tip").style.opacity = "0";
    svg.querySelector("#cross").setAttribute("opacity", "0");
    svg.querySelector("#dot").setAttribute("opacity", "0");
  };
}

async function tick(){
  let d;
  try { d = await (await fetch("/stats.json", {cache:"no-store"})).json(); }
  catch { $("connText").textContent = "view unreachable"; return; }

  const pill = $("conn");
  pill.className = "pill " + (d.connected ? "up" : "down");
  $("connText").textContent = d.connected ? "connected" : "waiting for gateway";
  $("meta").textContent =
    `port ${d.port} · up ${Math.round(d.uptime_s)} s · ${nf.format(d.batches)} batches` +
    (d.reconnects > 1 ? ` · ${d.reconnects} connections` : "") +
    (d.protocol_errs ? ` · ${d.protocol_errs} protocol errors` : "") +
    (d.silent ? ` · ${d.silent} unreported jumps` : "");

  rate = d.rate;
  $("k-rate").textContent   = nf.format(rate[rate.length - 2] || 0);
  $("k-frames").textContent = nf.format(d.frames);
  $("k-gaps").textContent   = nf.format(d.markers);
  $("k-missing").textContent= nf.format(d.missing);
  $("k-ids").textContent    = nf.format(d.ids.length);

  const L = d.latency;
  $("l-window").textContent = L.n && L.shared_clock
    ? `· rolling ${L.span_s.toFixed(0)} s, ${nf.format(L.n)} frames`
    : "";
  const shared = L.shared_clock;
  $("l-pcts").hidden = !shared;
  $("l-note").hidden = shared;
  if (shared) {
    const set = (k,v) => $(k).textContent = L.n ? ns(v) : "–";
    set("l-min",L.min); set("l-p50",L.p50); set("l-p90",L.p90);
    set("l-p99",L.p99); set("l-p999",L.p999); set("l-max",L.max);
  }

  const ids = d.ids.slice().sort((a,b) => b.count - a.count);
  const top = Math.max(1, ...ids.map(i => i.count));
  $("ids").innerHTML = ids.length ? ids.map(i =>
    `<tr><td class="mono">0x${i.id.toString(16).toUpperCase().padStart(3,"0")}</td>` +
    `<td>${i.src}</td><td class="num">${nf.format(i.count)}</td>` +
    `<td class="barcell"><span class="bar" style="width:${Math.max(2,(i.count/top)*112)}px"></span></td>` +
    `<td class="mono">${i.data || "—"}</td>` +
    `<td class="num">${age(i.age_ms)}</td></tr>`).join("")
    : `<tr><td colspan="6" class="empty">waiting for frames</td></tr>`;

  $("recent").innerHTML = d.recent.length ? d.recent.map(f =>
    `<tr><td class="num mono">${f.seq}</td>` +
    `<td class="mono">0x${f.id.toString(16).toUpperCase().padStart(3,"0")}</td>` +
    `<td>${f.src}</td><td class="mono">${f.data || "—"}</td></tr>`).join("")
    : `<tr><td colspan="4" class="empty">waiting for frames</td></tr>`;

  draw();
}

tick();
setInterval(tick, 500);
addEventListener("resize", draw);
</script></body></html>)PAGE";

void usage() {
  std::fprintf(stderr,
               "etg-view %s - gateway consumer with a live web view\n"
               "\n"
               "usage: etg-view [--host H] [--port P] [--http PORT]\n"
               "\n"
               "  --host HOST    gateway host (default 127.0.0.1)\n"
               "  --port PORT    gateway port to consume (default 9003)\n"
               "  --http PORT    port to serve the view on (default 8080)\n"
               "  --seconds N    run for N seconds (0 = until interrupted)\n",
               etg::version().data());
}

}  // namespace

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  std::uint16_t port = 9003;
  std::uint16_t http_port = 8080;
  int seconds = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const bool has_value = (i + 1) < argc;
    if (arg == "--host" && has_value) {
      host = argv[++i];
    } else if (arg == "--port" && has_value) {
      port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--http" && has_value) {
      http_port = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--seconds" && has_value) {
      seconds = std::atoi(argv[++i]);
    } else if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "unknown or incomplete argument: %s\n\n", arg.c_str());
      usage();
      return 2;
    }
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  State state;
  state.started_ns = etg::monotonic_ns();

  std::string error;
  auto server = etg::http::Server::create(http_port, error);
  if (!server) {
    std::fprintf(stderr, "http: %s\n", error.c_str());
    return 1;
  }

  server->start([&state, port](const std::string& path) -> etg::http::Response {
    if (path == "/stats.json") {
      return {200, "application/json", json_stats(state, port)};
    }
    if (path == "/" || path == "/index.html") {
      return {200, "text/html; charset=utf-8", kPage};
    }
    return {404, "text/plain", "not found"};
  });

  std::fprintf(stderr, "etg-view: consuming %s:%u, serving http://127.0.0.1:%u/\n", host.c_str(),
               port, server->port());

  const std::uint64_t deadline =
      seconds > 0 ? etg::monotonic_ns() + static_cast<std::uint64_t>(seconds) * 1'000'000'000ULL
                  : UINT64_MAX;
  std::uint64_t next_tick = etg::monotonic_ns() + 1'000'000'000ULL;
  std::vector<etg::Frame> batch;

  while (g_stop == 0 && etg::monotonic_ns() < deadline) {
    auto stream = etg::FrameStream::connect(host, port, error);
    if (!stream) {
      // Retrying rather than exiting: the view is meant to be left open across
      // gateway restarts, and a dashboard that dies with its subject is useless
      // exactly when something has gone wrong.
      std::this_thread::sleep_for(std::chrono::milliseconds{500});
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      state.connected = true;
      ++state.reconnects;
      state.gaps.reset();
    }

    while (g_stop == 0 && etg::monotonic_ns() < deadline) {
      const etg::FrameStream::Status st = stream->read_batch(batch, 200);
      if (st == etg::FrameStream::Status::kClosed) {
        break;
      }

      const std::uint64_t now = etg::monotonic_ns();
      {
        std::lock_guard<std::mutex> lock(state.mutex);

        if (st == etg::FrameStream::Status::kProtocolError) {
          ++state.protocol_errs;
        } else if (st == etg::FrameStream::Status::kOk && !batch.empty()) {
          ++state.batches;
          for (const etg::Frame& f : batch) {
            if (etg::wire::is_echo(f)) {
              static_cast<void>(stream->send_back(f));
              continue;
            }
            // Markers are loss reports, not frames: counting one as traffic
            // would show a bus carrying data that never existed.
            if (!state.gaps.observe(f)) {
              continue;
            }
            if (!state.have_seq) {
              state.first_seq = f.seq;
            }
            state.last_seq = f.seq;
            state.have_seq = true;
            ++state.frames;

            const std::int64_t delay = static_cast<std::int64_t>(now) -
                                       static_cast<std::int64_t>(f.t_ingest_ns);
            if (!etg::same_clock_domain(delay)) {
              ++state.off_domain;  // the gateway is not on this machine
            } else {
              state.latency.add(now, delay);
            }

            // Bounded: a fuzzed or misconfigured bus can present every one of
            // 2^29 identifiers, and an unbounded map would be a slow memory
            // leak dressed up as a feature.
            auto it = state.ids.find(f.can_id);
            if (it == state.ids.end() && state.ids.size() < kMaxIds) {
              it = state.ids.emplace(f.can_id, IdStat{}).first;
            }
            if (it != state.ids.end()) {
              IdStat& s = it->second;
              ++s.count;
              s.last_seen_ns = now;
              s.src_id = f.src_id;
              s.len = f.len;
              s.data = f.data;
            }

            state.recent.push_back(f);
            if (state.recent.size() > kRecentFrames) {
              state.recent.pop_front();
            }
          }
        }

        if (now >= next_tick) {
          next_tick = now + 1'000'000'000ULL;
          state.rate[state.rate_pos] = state.frames - state.frames_at_tick;
          state.frames_at_tick = state.frames;
          state.rate_pos = (state.rate_pos + 1) % kRateWindow;
        }
      }

      if (st == etg::FrameStream::Status::kProtocolError) {
        break;
      }
    }

    std::lock_guard<std::mutex> lock(state.mutex);
    state.connected = false;
  }

  server->stop();
  std::fprintf(stderr,
               "\netg-view: frames=%llu markers=%llu lost=%llu silent=%llu http_requests=%llu\n",
               static_cast<unsigned long long>(state.frames),
               static_cast<unsigned long long>(state.gaps.stats().markers),
               static_cast<unsigned long long>(state.gaps.total_lost()),
               static_cast<unsigned long long>(state.gaps.stats().silent_jumps),
               static_cast<unsigned long long>(server->requests()));
  return 0;
}
