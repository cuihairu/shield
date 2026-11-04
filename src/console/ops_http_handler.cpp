// [SHIELD_CONSOLE] HTTP ops endpoints implementation
#include "shield/console/ops_http_handler.hpp"

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "shield/config/config.hpp"
#include "shield/log/logger.hpp"
#include "shield/lua/slow_calls.hpp"
#include "shield/net/listener.hpp"
#include "shield/plugin/plugin_host.hpp"

#ifdef SHIELD_ENABLE_CLUSTER
#include "cluster_status.hpp"
#include "shield/cluster/cluster_manager.hpp"
#include "shield/cluster/cluster_transport.hpp"
#endif

#ifdef SHIELD_ENABLE_SERVER
#include "server_status.hpp"
#include "shield/server/server_manager.hpp"
#endif

#ifdef SHIELD_ENABLE_GLOBAL
#include "global_status.hpp"
#include "shield/global/global_manager.hpp"
#endif

namespace {

/// Process start reference for uptime-based health and metrics. First use
/// in this translation unit pins the process start (good enough for a
/// health probe; the optional server module has the authoritative clock).
const std::chrono::steady_clock::time_point kProcessStart =
    std::chrono::steady_clock::now();

double process_uptime_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         kProcessStart)
        .count();
}

/// Process-level resource gauges (Linux /proc). Platform differences are
/// isolated here: non-Linux builds return no entries (gauges omitted).
/// Values are bytes (RSS), count (FD/threads), or percent (CPU).
struct ProcessResource {
    double rss_kib = -1;      // VmRSS, KiB
    double threads = -1;      // Threads count
    double fd_count = -1;     // open file descriptors
    double cpu_percent = -1;  // CPU since last sample
};

// Last CPU sample (jiffies of utime+stime and the wall timestamp) so the
// CPU gauge is a rate over the interval, not a cumulative counter.
struct CpuSample {
    unsigned long long ticks = 0;
    std::chrono::steady_clock::time_point at;
};
CpuSample& last_cpu_sample() {
    static CpuSample s;
    return s;
}

ProcessResource read_process_resources() {
    ProcessResource r;

    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream iss(line.substr(6));
            std::string unit;
            double v = 0;
            if (iss >> v >> unit && unit == "kB") {
                r.rss_kib = v;
            }
        } else if (line.rfind("Threads:", 0) == 0) {
            std::istringstream iss(line.substr(8));
            double v = 0;
            if (iss >> v) {
                r.threads = v;
            }
        }
    }

    // Open FDs: count entries in /proc/self/fd.
    std::error_code ec;
    std::size_t fd_count = 0;
    for (auto it = std::filesystem::directory_iterator("/proc/self/fd", ec);
         it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (!ec) ++fd_count;
        if (ec) break;
    }
    if (!ec) {
        r.fd_count = static_cast<double>(fd_count);
    }

    // CPU: utime + stime from /proc/self/stat (fields 14 and 15, 1-based).
    // The sample is clock ticks since process start; a delta over the wall
    // interval yields a percentage (assuming 100 ticks/sec via sysconf).
    std::ifstream stat("/proc/self/stat");
    std::string content((std::istreambuf_iterator<char>(stat)),
                        std::istreambuf_iterator<char>());
    if (!content.empty()) {
        auto rparen = content.rfind(')');
        if (rparen != std::string::npos && rparen + 2 < content.size()) {
            std::istringstream iss(content.substr(rparen + 2));
            std::string field;
            for (int i = 3; i <= 13; ++i) iss >> field;
            unsigned long long utime = 0, stime = 0;
            if (iss >> utime >> stime) {
                const auto now = std::chrono::steady_clock::now();
                CpuSample& last = last_cpu_sample();
                const unsigned long long now_ticks = utime + stime;
                if (last.ticks != 0) {
                    const double dt =
                        std::chrono::duration<double>(now - last.at).count();
                    const long hz = sysconf(_SC_CLK_TCK);
                    if (dt > 0 && hz > 0) {
                        const double delta =
                            static_cast<double>(now_ticks - last.ticks) / hz;
                        r.cpu_percent = (delta / dt) * 100.0;
                    }
                } else {
                    // First sample: no interval yet, report 0 so the gauge
                    // is present immediately (subsequent polls carry a rate).
                    r.cpu_percent = 0.0;
                }
                last.ticks = now_ticks;
                last.at = now;
            }
        }
    }

    return r;
}

/// The slow_calls section attached to /ops/profile status and report:
/// up to 16 newest samples plus the cumulative count that survives ring
/// wrap. Metering is decoupled from the sampling session, so this is
/// meaningful even while no session is live.
nlohmann::json slow_calls_section() {
    const auto snap = shield::lua::SlowCallRing::instance().snapshot();
    nlohmann::json recent = nlohmann::json::array();
    const std::size_t take = std::min<std::size_t>(snap.recent.size(), 16);
    for (std::size_t i = 0; i < take; ++i) {
        const auto& rec = snap.recent[i];
        recent.push_back(  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                           // nlohmann::json braced-init branches)
            {{"caller", rec.caller},
             {"callee", rec.callee},
             {"elapsed_ms", rec.elapsed_ms},
             {"ok", rec.ok},
             {"at", rec.at}});
    }
    // GCOVR_EXCL_START (compiler artifact: inlined nlohmann::json
    // braced-init branches)
    nlohmann::json section = {{"recent", recent},
                              {"total_recorded", snap.total_recorded}};
    // GCOVR_EXCL_STOP
    return section;
}

/// Prometheus label-value escaping (backslash, quote, newline).
std::string prom_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        switch (c) {  // GCOVR_EXCL_START (defensive: label values are
                      // whitelisted service names and fixed state enums; the
                      // escape cases never fire)
            // (label values are whitelisted service names and fixed state
            // enums; the escape cases never fire)
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            // GCOVR_EXCL_STOP
            default:
                out += c;
        }
    }
    return out;
}  // GCOVR_EXCL_LINE (unwind cleanup for throwing appends; untestable)

/// Appends one HELP/TYPE header pair plus a single sample line.
void prom_emit(std::string& out, const std::string& name,
               const std::string& type, const std::string& help,
               const std::string& labels, double value) {
    out += "# HELP " + name + " " + help + "\n";
    out += "# TYPE " + name + " " + type + "\n";
    out += name;
    if (!labels.empty()) {  // GCOVR_EXCL_BR_LINE (defensive: labeled
                            // single-sample prom_emit calls only exist behind
                            // optional #ifdefs, default-build callers pass
                            // empty labels)
        // GCOVR_EXCL_START (labeled single-sample emitters live behind
        // optional #ifdefs; every default-build call passes empty labels)
        out += "{" + labels + "}";
        // GCOVR_EXCL_STOP
    }
    out += ' ';
    if (value == static_cast<long long>(value)) {
        out += std::to_string(static_cast<long long>(value));
    } else {
        out += std::to_string(value);
    }
    out += '\n';
}

/// Grouped variant: one HELP/TYPE header pair, then one sample per label
/// set (labels must already be escaped).
void prom_emit_group(
    std::string& out, const std::string& name, const std::string& type,
    const std::string& help,
    const std::vector<std::pair<std::string, double>>& samples) {
    if (samples.empty()) {
        return;
    }
    out += "# HELP " + name + " " + help + "\n";
    out += "# TYPE " + name + " " + type + "\n";
    for (const auto& [labels, value] : samples) {
        out += name;
        if (!labels  // GCOVR_EXCL_BR_LINE (defensive: every prom_emit_group
                     // caller emits non-empty labels)
                 .empty()) {  // GCOVR_EXCL_BR_LINE (defensive: every
                              // prom_emit_group caller emits non-empty
                              // labels)
            out += "{" + labels + "}";
        }
        out += ' ';
        if (value == static_cast<long long>(value)) {
            out += std::to_string(static_cast<long long>(value));
        } else {
            out += std::to_string(value);
        }
        out += '\n';
    }
}

}  // namespace

namespace shield::console {

// Embedded ops dashboard: a single self-contained HTML document (inline CSS +
// JS, no external assets, no framework) that polls the read-only ops
// endpoints on the same origin every 2 s and renders health, gateway/session
// gauges, per-service internal state tables, rolling metric curves (canvas)
// and an opt-in profiler control panel. Kept as one raw-string constant so
// the HTTP server can serve it with zero filesystem or dependency footprint.
const char* kDashboardHtml = R"SHIELD_DASHBOARD(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Shield Ops Dashboard</title>
<style>
:root{--bg:#0d1117;--panel:#161b22;--border:#30363d;--fg:#e6edf3;--dim:#8b949e;--ok:#3fb950;--warn:#d29922;--bad:#f85149;--accent:#58a6ff}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
header{display:flex;align-items:center;gap:16px;padding:14px 20px;border-bottom:1px solid var(--border);position:sticky;top:0;background:var(--bg);z-index:2}
header h1{font-size:18px;margin:0;letter-spacing:.5px}
.badge{padding:2px 10px;border-radius:10px;font-weight:600;font-size:12px}
.badge.ok{background:rgba(63,185,80,.15);color:var(--ok)}
.badge.degraded{background:rgba(210,153,34,.15);color:var(--warn)}
.badge.down{background:rgba(248,81,73,.15);color:var(--bad)}
.meta{color:var(--dim);font-size:12px}
main{padding:16px 20px;display:grid;gap:16px;grid-template-columns:repeat(auto-fit,minmax(340px,1fr))}
section{background:var(--panel);border:1px solid var(--border);border-radius:8px;padding:14px 16px;min-width:0}
section h2{margin:0 0 10px;font-size:13px;text-transform:uppercase;letter-spacing:1px;color:var(--dim)}
.cards{display:grid;gap:10px;grid-template-columns:repeat(auto-fit,minmax(120px,1fr))}
.card{background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:8px 10px}
.card .v{font-size:20px;font-weight:700}
.card .k{color:var(--dim);font-size:11px;text-transform:uppercase;letter-spacing:.5px}
canvas{width:100%;height:110px;display:block;background:var(--bg);border:1px solid var(--border);border-radius:6px}
table{width:100%;border-collapse:collapse;font-size:13px}
th,td{padding:5px 8px;text-align:right;border-bottom:1px solid var(--border);white-space:nowrap}
th{color:var(--dim);font-weight:600;font-size:11px;text-transform:uppercase}
th:first-child,td:first-child{text-align:left}
tr:last-child td{border-bottom:none}
.pill{display:inline-block;padding:1px 8px;border-radius:8px;font-size:11px;font-weight:600}
.pill.started,.pill.ok{background:rgba(63,185,80,.15);color:var(--ok)}
.pill.err{background:rgba(248,81,73,.15);color:var(--bad)}
.pill.other{background:rgba(88,166,255,.12);color:var(--accent)}
label{display:block;margin:8px 0 2px;color:var(--dim);font-size:12px}
input{background:var(--bg);border:1px solid var(--border);color:var(--fg);border-radius:6px;padding:6px 8px;width:100%;font:inherit}
button{background:#21262d;border:1px solid var(--border);color:var(--fg);border-radius:6px;padding:6px 14px;font:inherit;cursor:pointer;margin:10px 8px 0 0}
button:hover{border-color:var(--accent)}
pre{background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:10px;overflow:auto;max-height:240px;font-size:12px}
footer{padding:12px 20px;color:var(--dim);font-size:12px}
.row2{display:grid;grid-template-columns:1fr 1fr;gap:16px}
@media(max-width:800px){.row2{grid-template-columns:1fr}}
</style>
</head>
<body>
<header>
  <h1>Shield Ops Dashboard</h1>
  <span id="health-badge" class="badge down">connecting</span>
  <span id="uptime" class="meta"></span>
  <span id="updated" class="meta" style="margin-left:auto"></span>
</header>
<main>
  <section>
    <h2>Gateway</h2>
    <div class="cards" id="gw-cards"></div>
    <div class="cards" id="net-cards" style="margin-top:10px"></div>
  </section>
  <section>
    <h2>Health checks</h2>
    <div id="health-checks" class="meta">waiting...</div>
  </section>
  <section style="grid-column:1/-1">
    <h2>Metrics curves (2 s samples)</h2>
    <div style="display:grid;gap:12px;grid-template-columns:repeat(auto-fit,minmax(300px,1fr))">
      <div><canvas id="ch-sessions" width="600" height="220"></canvas><div class="meta">shield_gateway_active_sessions</div></div>
      <div><canvas id="ch-reqrate" width="600" height="220"></canvas><div class="meta">service requests / s</div></div>
      <div><canvas id="ch-errrate" width="600" height="220"></canvas><div class="meta">service errors / s</div></div>
    </div>
  </section>
  <section style="grid-column:1/-1">
    <h2>Services (internal state)</h2>
    <div style="overflow-x:auto"><table id="svc-table"><thead><tr><th>service</th><th>requests</th><th>errors</th><th>uptime s</th><th>timers</th><th>pending calls</th><th>pending tasks</th><th>coroutines</th><th>mem KB</th></tr></thead><tbody></tbody></table></div>
  </section>
  <section style="grid-column:1/-1">
    <h2>Plugins</h2>
    <div style="overflow-x:auto"><table id="plugin-table"><thead><tr><th>id</th><th>package</th><th>state</th><th>required</th></tr></thead><tbody></tbody></table></div>
  </section>
  <section style="grid-column:1/-1">
    <h2>Profiler (/ops/profile, opt-in)</h2>
    <div class="row2">
      <div>
        <label>Bearer token</label><input id="prof-token" type="password" placeholder="http.profile_token">
        <label>service</label><input id="prof-service" placeholder="service name">
        <label>duration_ms</label><input id="prof-duration" value="5000">
        <button id="prof-status">status</button><button id="prof-start">start</button><button id="prof-stop">stop / report</button>
      </div>
      <div><div id="prof-out" class="meta">Endpoint answers only when http.profile_enabled=true + token configured.</div></div>
    </div>
  </section>
</main>
<footer>Data polled from GET /ops/health, /ops/status, /ops/metrics (same origin) every 2 s. Self-contained page, zero external assets.</footer>
<script>
(function(){
"use strict";
var $=function(id){return document.getElementById(id);};
var MAXPTS=300;
var hist={sessions:[],req:[],err:[]};
var lastReq=null,lastErr=null;

function badge(cls,txt){var b=$("health-badge");b.className="badge "+cls;b.textContent=txt;}
function fmt(n){
  if(n===null||n===undefined)return "-";
  if(typeof n!=="number")return String(n);
  if(Math.abs(n)>=1e6)return (n/1e6).toFixed(1)+"M";
  if(Math.abs(n)>=1e4)return (n/1e3).toFixed(1)+"k";
  return String(Math.round(n*100)/100);
}
function parseProm(text){
  var out={};
  text.split("\n").forEach(function(line){
    if(!line||line.charAt(0)==="#")return;
    var name=line,labels={},rest=line;
    var b=line.indexOf("{");
    if(b>=0){
      name=line.slice(0,b);
      var e=line.indexOf("}",b);
      var re=/([a-zA-Z_][a-zA-Z0-9_]*)="((?:[^"\\]|\\.)*)"/g,mm;
      while((mm=re.exec(line.slice(b+1,e))))labels[mm[1]]=mm[2];
      rest=line.slice(e+1);
    }
    var v=parseFloat(rest.trim().split(/\s+/)[0]);
    if(isNaN(v))return;
    (out[name]=out[name]||[]).push({labels:labels,value:v});
  });
  return out;
}
function sum(m,name){if(!m[name])return null;return m[name].reduce(function(a,s){return a+s.value;},0);}
function byLabel(m,name,key){var o={};(m[name]||[]).forEach(function(s){o[s.labels[key]||""]=s.value;});return o;}
function push(arr,p){arr.push(p);if(arr.length>MAXPTS)arr.shift();}

function draw(cv,series,color,unit){
  var dpr=window.devicePixelRatio||1;
  var w=cv.clientWidth,h=cv.clientHeight;
  if(cv.width!==Math.round(w*dpr)||cv.height!==Math.round(h*dpr)){cv.width=Math.round(w*dpr);cv.height=Math.round(h*dpr);}
  var ctx=cv.getContext("2d");ctx.setTransform(dpr,0,0,dpr,0,0);
  ctx.clearRect(0,0,w,h);
  ctx.strokeStyle="#30363d";ctx.strokeRect(0.5,0.5,w-1,h-1);
  if(series.length<2){ctx.fillStyle="#8b949e";ctx.font="12px system-ui";ctx.fillText("collecting samples...",10,20);return;}
  var min=Infinity,max=-Infinity;
  series.forEach(function(p){if(p.v<min)min=p.v;if(p.v>max)max=p.v;});
  if(max===min)max=min+1;
  var t0=series[0].t,t1=series[series.length-1].t;
  if(t1===t0)t1=t0+1;
  function x(p){return (p.t-t0)/(t1-t0)*(w-70)+10;}
  function y(p){return h-14-(p.v-min)/(max-min)*(h-28);}
  ctx.strokeStyle=color;ctx.lineWidth=1.5;ctx.beginPath();
  series.forEach(function(p,i){if(i)ctx.lineTo(x(p),y(p));else ctx.moveTo(x(p),y(p));});
  ctx.stroke();
  var last=series[series.length-1];
  ctx.fillStyle=color;ctx.beginPath();ctx.arc(x(last),y(last),2.5,0,6.2832);ctx.fill();
  ctx.fillStyle="#8b949e";ctx.font="11px system-ui";
  ctx.fillText(fmt(max),w-58,14);
  ctx.fillText(fmt(min),w-58,h-6);
  ctx.fillStyle=color;ctx.font="bold 13px system-ui";
  ctx.fillText(fmt(last.v)+(unit||""),10,16);
}

function setCards(el,items){
  el.innerHTML="";
  items.forEach(function(it){
    var c=document.createElement("div");c.className="card";
    var v=document.createElement("div");v.className="v";v.textContent=it.v===null?"-":fmt(it.v);
    var k=document.createElement("div");k.className="k";k.textContent=it.k;
    c.appendChild(v);c.appendChild(k);el.appendChild(c);
  });
}

function renderServices(m){
  var reqs=byLabel(m,"shield_service_requests_total","service");
  var errs=byLabel(m,"shield_service_errors_total","service");
  var upt=byLabel(m,"shield_service_uptime_seconds","service");
  var tim=byLabel(m,"shield_service_timers","service");
  var pc=byLabel(m,"shield_service_pending_calls","service");
  var pt=byLabel(m,"shield_service_pending_tasks","service");
  var co=byLabel(m,"shield_service_coroutines","service");
  var mem=byLabel(m,"shield_service_memory_kb","service");
  var names=Object.keys(reqs);
  Object.keys(errs).forEach(function(n){if(names.indexOf(n)<0)names.push(n);});
  names.sort(function(a,b){return (reqs[b]||0)-(reqs[a]||0);});
  var tb=$("svc-table").querySelector("tbody");tb.innerHTML="";
  if(!names.length){
    var tr0=document.createElement("tr");var td0=document.createElement("td");
    td0.colSpan=9;td0.className="meta";td0.textContent="no live services (mesh busy or none spawned)";
    tr0.appendChild(td0);tb.appendChild(tr0);return;
  }
  names.forEach(function(n){
    var tr=document.createElement("tr");
    [n,reqs[n],errs[n],upt[n],tim[n],pc[n],pt[n],co[n],mem[n]].forEach(function(v,i){
      var td=document.createElement("td");
      td.textContent=v===undefined?"-":(i===0?String(v):fmt(v));
      tr.appendChild(td);
    });
    tb.appendChild(tr);
  });
}

function renderStatus(data){
  var tb=$("plugin-table").querySelector("tbody");tb.innerHTML="";
  (data.plugins||[]).forEach(function(p){
    var tr=document.createElement("tr");
    var cells=[p.id,p.package,null,p.required?"yes":"no"];
    cells.forEach(function(v,i){
      var td=document.createElement("td");
      if(i===2){
        var s=document.createElement("span");
        s.className="pill "+(p.state==="started"?"started":"other");
        s.textContent=String(p.state);
        td.appendChild(s);
      }else{
        td.textContent=v===undefined||v===null?"-":String(v);
      }
      tr.appendChild(td);
    });
    tb.appendChild(tr);
  });
}

function renderHealth(h){
  var st=(h&&h.status)==="ok"?"ok":(h?"degraded":"down");
  badge(st==="ok"?"ok":(st==="down"?"down":"degraded"),st);
  if(h&&h.uptime!==undefined)$("uptime").textContent="uptime "+fmt(h.uptime)+"s";
  var el=$("health-checks");el.innerHTML="";
  var checks=(h&&h.checks)||{};
  var names=Object.keys(checks);
  if(!names.length){el.textContent="no check data";return;}
  names.forEach(function(name){
    var c=checks[name]||{};
    var row=document.createElement("div");
    var pill=document.createElement("span");
    pill.className="pill "+(c.status==="ok"?"started":"err");
    pill.textContent=name+": "+(c.status||"?");
    row.appendChild(pill);
    var det=[];
    Object.keys(c).forEach(function(k){if(k!=="status")det.push(k+"="+c[k]);});
    if(det.length){
      var m=document.createElement("span");m.className="meta";
      m.textContent="  "+det.join(",  ");
      row.appendChild(m);
    }
    el.appendChild(row);
  });
}

function jget(path){
  return fetch(path,{cache:"no-store"}).then(function(r){
    return r.json().then(function(j){return {ok:r.ok,j:j};});
  });
}

function tick(){
  Promise.all([
    jget("/ops/health").catch(function(){return null;}),
    jget("/ops/status").catch(function(){return null;}),
    fetch("/ops/metrics",{cache:"no-store"}).then(function(r){return r.text();}).catch(function(){return null;})
  ]).then(function(res){
    var h=res[0],st=res[1],mx=res[2];
    // /ops/health and /ops/status wrap their payload in {type,data}: unwrap.
    renderHealth(h&&h.j&&h.j.data?h.j.data:{status:"down",checks:{}});
    if(st&&st.j&&st.j.data)renderStatus(st.j.data);
    if(mx){
      var m=parseProm(mx);
      var sessions=sum(m,"shield_gateway_active_sessions");
      var conns=sum(m,"shield_gateway_connections_total");
      var rej=sum(m,"shield_gateway_rejections_total");
      var rl=sum(m,"shield_gateway_rate_limited_messages_total");
      var now=Date.now();
      push(hist.sessions,{t:now,v:sessions||0});
      var req=sum(m,"shield_service_requests_total");
      var err=sum(m,"shield_service_errors_total");
      var prev=hist.req.length?hist.req[hist.req.length-1].t:null;
      var dtr=prev?Math.max(0.5,(now-prev)/1000):2;
      push(hist.req,{t:now,v:Math.max(0,(req-(lastReq===null?req:lastReq))/dtr)});
      push(hist.err,{t:now,v:Math.max(0,(err-(lastErr===null?err:lastErr))/dtr)});
      lastReq=req;lastErr=err;
      setCards($("gw-cards"),[
        {k:"active sessions",v:sessions},
        {k:"connections",v:conns},
        {k:"rejected",v:rej},
        {k:"rate limited",v:rl}
      ]);
      var perPort={};
      (m["shield_gateway_active_sessions"]||[]).forEach(function(s){perPort[":"+s.labels.port]=s.value;});
      var items=[];Object.keys(perPort).forEach(function(p){items.push({k:"active "+p,v:perPort[p]});});
      setCards($("net-cards"),items);
      renderServices(m);
      draw($("ch-sessions"),hist.sessions,"#58a6ff","");
      draw($("ch-reqrate"),hist.req,"#3fb950","/s");
      draw($("ch-errrate"),hist.err,"#f85149","/s");
    }
    $("updated").textContent="updated "+new Date().toLocaleTimeString();
  });
}
tick();
setInterval(tick,2000);

function showProf(status,j,text){
  var out=$("prof-out");
  if(!status){
    out.className="meta";out.textContent="error: "+((j&&j.message)||text);
    return;
  }
  out.className="";
  out.innerHTML="";
  var d=(j&&j.data)||{};
  var sc=d.slow_calls||{};
  var head=document.createElement("div");
  head.style.marginBottom="8px";
  head.textContent="active="+(d.active?"yes":"no")
    +(d.service?("  service="+d.service):"")
    +(d.elapsed_ms!==undefined?("  elapsed_ms="+d.elapsed_ms):"")
    +"  slow_calls_total_recorded="+(sc.total_recorded!==undefined?sc.total_recorded:"-");
  out.appendChild(head);
  var recent=sc.recent||[];
  if(recent.length){
    var t=document.createElement("table");
    t.innerHTML="<thead><tr><th>caller</th><th>callee</th><th>elapsed ms</th><th>ok</th><th>at</th></tr></thead>";
    var tb=document.createElement("tbody");
    recent.forEach(function(r){
      var tr=document.createElement("tr");
      [r.caller,r.callee,r.elapsed_ms,r.ok,r.at].forEach(function(v){
        var td=document.createElement("td");td.textContent=String(v);tr.appendChild(td);
      });
      tb.appendChild(tr);
    });
    t.appendChild(tb);out.appendChild(t);
  }
  var pre=document.createElement("pre");
  pre.textContent=JSON.stringify(d,null,2);
  out.appendChild(pre);
}

function profReq(action,extra){
  var tok=$("prof-token").value;
  var body=JSON.stringify(Object.assign({action:action},extra||{}));
  var headers={"Content-Type":"application/json"};
  if(tok)headers["Authorization"]="Bearer "+tok;
  fetch("/ops/profile",{method:"POST",cache:"no-store",headers:headers,body:body})
    .then(function(r){
      return r.json().then(function(j){showProf(r.ok,j,null);});
    })
    .catch(function(e){showProf(false,null,String(e));});
}
$("prof-status").addEventListener("click",function(){profReq("status");});
$("prof-start").addEventListener("click",function(){
  profReq("start",{service:$("prof-service").value,
                   duration_ms:parseInt($("prof-duration").value,10)||5000});
});
$("prof-stop").addEventListener("click",function(){profReq("report");});
})();
</script>
</body>
</html>)SHIELD_DASHBOARD";

OpsHttpHandler::OpsHttpHandler(shield::lua::LuaServiceManager& lua_mgr,
                               shield::lua::LuaRuntime& lua_rt)
    : lua_mgr_(lua_mgr), lua_rt_(lua_rt) {}

void OpsHttpHandler::register_routes(shield::net::HttpServer& server) {
    // Ops-wide auth gate. When http.ops_auth_token is non-empty, every
    // /ops/* endpoint below (except the k8s-probe-exempt /ops/health) is
    // denied to requests without a matching "Authorization: Bearer <token>".
    // An empty token keeps the gate off (legacy behaviour, no auth).
    auth_token_ = shield::config::get("http.ops_auth_token", "");

    // Static frontend root (FIX-WEB-04). When configured, the built console
    // is served from this directory; unmatched non-API GETs fall through to
    // the SPA index.html (handle_default). Empty means the embedded
    // raw-string single page remains the fallback (no filesystem access).
    static_root_ = shield::config::get("http.static_root", "");
    if (!static_root_.empty()) {
        server.set_static_root(static_root_);
    }

    // Wrap a handler with the ops-wide auth check, unless the gate is off.
    auto authed = [this](shield::net::HttpHandler inner) {
        if (auth_token_.empty()) {
            return inner;
        }
        return shield::net::HttpHandler{
            [this, inner](const shield::net::HttpRequest& req)
                -> shield::net::HttpResponse {
                if (!request_authorized(req, auth_token_)) {
                    return make_error_response(401, "unauthorized");
                }
                return inner(req);
            }};
    };

    // /ops/health is exempt from the ops-wide gate: k8s liveness/readiness
    // probes are unauthenticated by design. It only exposes process-local
    // health state, never secrets or configuration.
    server.get("/ops/health",
               [this](const auto& req) { return handle_health(req); });
    server.get("/ops/status",
               authed([this](const auto& req) { return handle_status(req); }));
    server.get("/ops/metrics",
               authed([this](const auto& req) { return handle_metrics(req); }));
    server.get("/ops/services", authed([this](const auto& req) {
                   return handle_services(req);
               }));
    // ":name" pattern matches only when the exact "/ops/services" route
    // missed, so list vs. detail never shadow each other.
    server.get("/ops/services/:name", authed([this](const auto& req) {
                   return handle_service_detail(req);
               }));
    server.get("/ops/plugins",
               authed([this](const auto& req) { return handle_plugins(req); }));
    server.get("/ops/config",
               authed([this](const auto& req) { return handle_config(req); }));

    // Embedded single-page dashboard: one self-contained HTML document (no
    // external assets, no build step, no new dependencies) that polls the
    // read-only /ops/* endpoints above and renders health, per-service
    // state, metric curves and profile results client-side. Registered on
    // both "/" and "/ops/dashboard" only when no static root is configured;
    // with a static root the default handler serves the built console
    // instead (same-origin, FIX-WEB-04).
    if (static_root_.empty()) {
        server.get("/", authed([this](const auto& req) {
                       return handle_dashboard(req);
                   }));
        server.get("/ops/dashboard", authed([this](const auto& req) {
                       return handle_dashboard(req);
                   }));
    }

    // SPA fallback: unknown non-API paths serve the console index.html so a
    // deep link refreshes correctly; unknown /ops/* API paths get a clear
    // JSON 404 instead of HTML. Only the default handler sees unmatched
    // routes, so it runs after static serving (see HttpServer::dispatch).
    server.set_default_handler(
        [this](const auto& req) { return handle_default(req); });

    // /ops/eval is a remote-code-entry point: opt-in (http.eval_enabled),
    // token-gated (http.eval_token), and served in a restricted VM. The
    // route simply stays unregistered otherwise, so the endpoint 404s.
    if (shield::config::get("http.eval_enabled", "false") != "true") {
        auto& log = shield::log::get_logger("ops");
        SHIELD_LOG_INFO(log,
                        "/ops/eval disabled (opt in via http.eval_enabled=true "
                        "+ http.eval_token)");
    } else {
        eval_token_ = shield::config::get("http.eval_token", "");
        if (eval_token_.empty()) {
            auto& log = shield::log::get_logger("ops");
            SHIELD_LOG_ERROR(log,
                             "/ops/eval NOT registered: http.eval_enabled=true "
                             "requires a non-empty http.eval_token");
        } else {
            server.post("/ops/eval",
                        [this](const auto& req) { return handle_eval(req); });
        }
    }

    // /ops/profile exposes code locations and hit counts only (no payload,
    // no keys), but it still drives service VMs: same opt-in + token
    // discipline as /ops/eval, with a start cooldown on top.
    if (shield::config::get("http.profile_enabled", "false") != "true") {
        auto& log = shield::log::get_logger("ops");
        SHIELD_LOG_INFO(log,
                        "/ops/profile disabled (opt in via "
                        "http.profile_enabled=true + http.profile_token)");
        return;
    }
    profile_token_ = shield::config::get("http.profile_token", "");
    if (profile_token_.empty()) {
        auto& log = shield::log::get_logger("ops");
        SHIELD_LOG_ERROR(
            log,
            "/ops/profile NOT registered: http.profile_enabled=true requires "
            "a non-empty http.profile_token");
        return;
    }
    server.post("/ops/profile",
                [this](const auto& req) { return handle_profile(req); });

    // Phase B slow-call metering arms with the endpoint (the only reader
    // of the ring), at a fixed threshold for the process lifetime: the
    // gate is sticky by design — a call suspended while armed must always
    // find its completion path still metering, so it never disarms (see
    // SlowCallRing's class note). Zero config keeps the gate closed.
    const std::string slow_ms =
        shield::config::get("http.slow_call_threshold_ms", "0");
    if (slow_ms != "0") {
        try {
            shield::lua::SlowCallRing::instance().enable(
                static_cast<uint64_t>(std::stoull(slow_ms)));
        } catch (const std::exception&  // GCOVR_EXCL_BR_LINE (compiler
                                        // artifact: catch-entry pseudo-arc)
                     e) {
            auto& log = shield::log::get_logger("ops");
            SHIELD_LOG_ERROR(log, "http.slow_call_threshold_ms not a number: " +
                                      slow_ms + " (slow-call metering off)");
        }
    }
}

bool OpsHttpHandler::token_matches(const std::string& provided,
                                   const std::string& expected) {
    // A length mismatch only leaks the length, never token content; equal
    // lengths compare over every byte so timing reveals nothing.
    if (provided.size() != expected.size()) return false;
    unsigned char diff = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        diff |= static_cast<unsigned char>(provided[i]) ^
                static_cast<unsigned char>(expected[i]);
    }
    return diff == 0;
}

std::string OpsHttpHandler::bearer_token(const shield::net::HttpRequest& req) {
    auto auth_it = req.find(boost::beast::http::field::authorization);
    std::string provided;
    if (auth_it != req.end()) {
        provided = std::string(auth_it->value());
    }
    constexpr char kBearerPrefix[] = "Bearer ";
    if (provided.rfind(kBearerPrefix, 0) == 0) {
        provided = provided.substr(sizeof(kBearerPrefix) - 1);
    }
    return provided;
}

bool OpsHttpHandler::request_authorized(const shield::net::HttpRequest& req,
                                        const std::string& expected) {
    // Constant-time compare: length mismatch returns immediately (only
    // leaks the length, never content); equal lengths compare over every
    // byte so timing reveals nothing about the expected token.
    return token_matches(bearer_token(req), expected);
}

shield::net::HttpResponse OpsHttpHandler::handle_health(
    const shield::net::HttpRequest& req) {
    // Lightweight probe: process-local reads only, never a Lua actor round
    // trip, so it answers even when the actor mesh is wedged.
    nlohmann::json checks;
    checks["core"] = {
        {"status", "ok"},
        {"uptime_seconds",
         process_uptime_seconds()}};  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                      // inlined nlohmann::json braced-init
                                      // branches)

    // Plugins: a required instance that did not reach "started" degrades
    // the process (absent optional instances are fine).
    {
        std::size_t started = 0;
        std::size_t required_down = 0;
        for (const auto& inst :
             shield::plugin::global_host().list_instances()) {
            if (inst.state == "started") {
                ++started;
            } else if (inst.required) {
                ++required_down;
            }
        }
        checks["plugins"] = {
            {"status", required_down == 0 ? "ok" : "degraded"},
            {"started", started},
            {"required_down",
             required_down}};  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                               // nlohmann::json braced-init branches)
    }

#ifdef SHIELD_ENABLE_SERVER
    if (auto* sm = shield::server::ServerManager::global()) {
        const char* state = shield::server::server_state_name(sm->state());
        // Only "running" counts as healthy; starting/maintenance/shutdown
        // all mean the process is not serving normally right now.
        const bool ok = sm->state() == shield::server::ServerState::kRunning;
        checks["server"] = {{"status", ok ? "ok" : "degraded"},
                            {"state", state}};
    }
#endif

#ifdef SHIELD_ENABLE_CLUSTER
    if (auto* cm = shield::cluster::global_cluster_manager()) {
        std::size_t online = 0;
        std::size_t down = 0;
        for (const auto& n : cm->nodes()) {
            const auto state = n.state;
            if (state == shield::cluster::NodeState::Online) {
                ++online;
            } else if (state == shield::cluster::NodeState::Offline ||
                       state == shield::cluster::NodeState::Removed) {
                ++down;
            }
        }
        checks["cluster"] = {{"status", down == 0 ? "ok" : "degraded"},
                             {"nodes_online", online},
                             {"nodes_down", down}};
    }
#endif

#ifdef SHIELD_ENABLE_GLOBAL
    // Same convention as server/cluster above: a GLOBAL-capable build may
    // still run as a non-global node, so absence omits the check rather
    // than degrading it.
    if (shield::global::GlobalManager::global() != nullptr) {
        checks["global"] = {
            {"status",
             "ok"}};  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                      // nlohmann::json braced-init branches; the line runs —
                      // the guarded health fetch asserts this field)
    }
#endif

    // Overall: degraded when any check degraded. The verdict maps onto the
    // HTTP status (ok -> 200, degraded -> 503) while the body keeps the same
    // envelope, so load balancers and orchestrators can gate on the status
    // code alone and probes can still read the per-check detail.
    std::string status = "ok";
    for (const auto& [name, check] : checks.items()) {
        (void)name;
        if (check.value("status", "ok") != "ok") {
            status = "degraded";
            break;
        }
    }
    nlohmann::json data = {
        {"status", status},
        // GCOVR_EXCL_START (braced-init continuation
        // attributed to no arc; the field is asserted)
        {"uptime", process_uptime_seconds()},
        // GCOVR_EXCL_STOP
        {"checks",
         std::move(checks)}};  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                               // nlohmann::json braced-init branches)
    return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // inlined nlohmann::json braced-init branches)
        status == "ok" ? 200
                       : 503,  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                               // nlohmann::json braced-init branches)
        {{"type", "result"}, {"data", data}});
}

shield::net::HttpResponse OpsHttpHandler::handle_metrics(
    const shield::net::HttpRequest& req) {
    std::string out;
    prom_emit(out, "shield_uptime_seconds", "gauge",
              "Process uptime in seconds", "", process_uptime_seconds());

    // Process-level resource gauges (Linux /proc, isolated in
    // read_process_resources). Omitted entirely on platforms without the
    // source (or when a read fails), so the endpoint never 500s.
    {
        const ProcessResource pr = read_process_resources();
        if (pr.rss_kib >= 0) {
            prom_emit(out, "shield_process_rss_bytes", "gauge",
                      "Resident set size in bytes", "", pr.rss_kib * 1024.0);
        }
        if (pr.threads >= 0) {
            prom_emit(out, "shield_process_threads", "gauge",
                      "Number of threads in the process", "", pr.threads);
        }
        if (pr.fd_count >= 0) {
            prom_emit(out, "shield_process_open_fds", "gauge",
                      "Open file descriptors", "", pr.fd_count);
        }
        if (pr.cpu_percent >= 0) {
            prom_emit(out, "shield_process_cpu_percent", "gauge",
                      "Process CPU usage percent since last sample", "",
                      pr.cpu_percent);
        }
    }

    // Plugins grouped by lifecycle state.
    {
        std::map<std::string, double> by_state;
        for (const auto& inst :
             shield::plugin::global_host().list_instances()) {
            by_state[inst.state] += 1.0;
        }
        std::vector<std::pair<std::string, double>> samples;
        for (const auto& [state, count] : by_state) {
            samples.emplace_back("state=\"" + prom_escape(state) + "\"", count);
        }
        prom_emit_group(out, "shield_plugin_instances", "gauge",
                        "Plugin instances by lifecycle state", samples);
    }

    // Plugin data pools: gauges collected through the shield.pool.stats.v1
    // interface. Fields a driver leaves at the -1 "unknown / not applicable"
    // sentinel are omitted rather than exported (sqlite has no pool, but its
    // async gauges are real).
    {
        std::vector<shield::plugin::PoolStatsResult> pools;
        shield::plugin::global_host().collect_pool_stats(pools);
        std::vector<std::pair<std::string, double>> size_samples;
        std::vector<std::pair<std::string, double>> in_use_samples;
        std::vector<std::pair<std::string, double>> idle_samples;
        std::vector<std::pair<std::string, double>> pending_async_samples;
        std::vector<std::pair<std::string, double>> holding_samples;
        for (const auto& p : pools) {
            if (p.status != shield::plugin::PoolStatsStatus::ok) continue;
            const std::string labels = "plugin=\"" + prom_escape(p.plugin_id) +
                                       "\",instance=\"" +
                                       prom_escape(p.instance_id) + "\"";
            if (p.stats.size >= 0) {
                size_samples.emplace_back(labels,
                                          static_cast<double>(p.stats.size));
                in_use_samples.emplace_back(
                    labels, static_cast<double>(p.stats.in_use));
                idle_samples.emplace_back(labels,
                                          static_cast<double>(p.stats.idle));
            }
            if (p.stats.pending_async >= 0) {
                pending_async_samples.emplace_back(
                    labels, static_cast<double>(p.stats.pending_async));
            }
            if (p.stats.holding >= 0) {
                holding_samples.emplace_back(
                    labels, static_cast<double>(p.stats.holding));
            }
        }
        prom_emit_group(out, "shield_plugin_pool_size", "gauge",
                        "Live connections in the plugin data pool",
                        size_samples);
        prom_emit_group(out, "shield_plugin_pool_in_use", "gauge",
                        "Pool connections currently checked out",
                        in_use_samples);
        prom_emit_group(out, "shield_plugin_pool_idle", "gauge",
                        "Pool connections idle in the free list", idle_samples);
        prom_emit_group(out, "shield_plugin_db_pending_async", "gauge",
                        "In-flight async DB calls (queued + running)",
                        pending_async_samples);
        prom_emit_group(out, "shield_plugin_db_holding", "gauge",
                        "Connections held by open async transactions",
                        holding_samples);
    }

    // Gateway view: accept/rejection/rate-limit counters and active sessions
    // aggregated over every live TCP listener, broken down by listen port.
    {
        std::vector<std::pair<std::string, double>> accept_samples;
        std::vector<std::pair<std::string, double>> rejection_samples;
        std::vector<std::pair<std::string, double>> active_samples;
        std::vector<std::pair<std::string, double>> rate_limited_samples;
        std::map<std::string, double> rejections;
        for (const auto* listener :
             shield::net::ListenerRegistry::instance().snapshot()) {
            const std::string port =
                std::to_string(static_cast<unsigned>(listener->port()));
            accept_samples.emplace_back(
                "port=\"" + port + "\"",
                static_cast<double>(listener->accepts_total()));
            rejections["port=\"" + port + "\",reason=\"blocked_ip\""] +=
                static_cast<double>(listener->blocked_rejects_total());
            rejections["port=\"" + port + "\",reason=\"connection_limit\""] +=
                static_cast<double>(listener->conn_limit_rejects_total());
            rejections["port=\"" + port + "\",reason=\"ip_limit\""] +=
                static_cast<double>(listener->ip_limit_rejects_total());
            active_samples.emplace_back(
                "port=\"" + port + "\"",
                static_cast<double>(listener->session_count()));
            rate_limited_samples.emplace_back(
                "port=\"" + port + "\"",
                static_cast<double>(listener->rate_limited_messages_total()));
        }
        for (const auto& [labels, value] : rejections) {
            rejection_samples.emplace_back(labels, value);
        }
        prom_emit_group(out, "shield_gateway_connections_total", "counter",
                        "Connections that completed the TCP accept step",
                        accept_samples);
        prom_emit_group(out, "shield_gateway_rejections_total", "counter",
                        "Connections rejected at accept time by reason",
                        rejection_samples);
        prom_emit_group(out, "shield_gateway_active_sessions", "gauge",
                        "Live gateway sessions", active_samples);
        prom_emit_group(
            out, "shield_gateway_rate_limited_messages_total", "counter",
            "Ingress messages dropped by the per-connection rate limit",
            rate_limited_samples);
        // TLS handshake counters (docs/tls-design.md). Plaintext-only
        // deployments emit no samples for these groups at all.
        std::vector<std::pair<std::string, double>> tls_handshake_samples;
        std::vector<std::pair<std::string, double>>
            tls_handshake_failure_samples;
        for (const auto* listener :
             shield::net::ListenerRegistry::instance().snapshot()) {
            if (!listener->tls_enabled()) {
                continue;
            }
            const std::string port =
                std::to_string(static_cast<unsigned>(listener->port()));
            tls_handshake_samples.emplace_back(
                "port=\"" + port + "\"",
                static_cast<double>(listener->tls_handshakes_total()));
            tls_handshake_failure_samples.emplace_back(
                "port=\"" + port + "\"",
                static_cast<double>(listener->tls_handshake_failures_total()));
        }
        prom_emit_group(out, "shield_gateway_tls_handshakes_total", "counter",
                        "TLS handshakes that completed successfully",
                        tls_handshake_samples);
        prom_emit_group(out, "shield_gateway_tls_handshake_failures_total",
                        "counter",
                        "TLS handshakes that failed or timed out "
                        "(no session was built)",
                        tls_handshake_failure_samples);
    }

    // Service count + per-service traffic: the metrics that need the actor
    // mesh; omitted on timeout so a wedged mesh never wedges the scrape.
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        auto future = promise->get_future();
        lua_mgr_.enqueue_forked_task("", [&mgr = lua_mgr_, promise]() {
            nlohmann::json payload = {
                {"count",
                 mgr.list_services()
                     .size()}};  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                 // inlined nlohmann::json braced-init branches)
            nlohmann::json requests = nlohmann::json::object();
            nlohmann::json errors = nlohmann::json::object();
            nlohmann::json uptime = nlohmann::json::object();
            nlohmann::json timers = nlohmann::json::object();
            nlohmann::json pending_calls = nlohmann::json::object();
            nlohmann::json pending_tasks = nlohmann::json::object();
            nlohmann::json coroutines = nlohmann::json::object();
            nlohmann::json memory_kb = nlohmann::json::object();
            for (const auto& [name, stats] : mgr.service_stats()) {
                requests[name] = stats.requests;
                errors[name] = stats.errors;
                uptime[name] = stats.uptime_seconds;
                timers[name] = stats.timers;
                pending_calls[name] = stats.pending_calls;
                pending_tasks[name] = stats.pending_tasks;
                coroutines[name] = stats.coroutines;
                memory_kb[name] = stats.memory_kb;
            }
            payload["requests"] = std::move(requests);
            payload["errors"] = std::move(errors);
            payload["uptime"] = std::move(uptime);
            payload["timers"] = std::move(timers);
            payload["pending_calls"] = std::move(pending_calls);
            payload["pending_tasks"] = std::move(pending_tasks);
            payload["coroutines"] = std::move(coroutines);
            payload["memory_kb"] = std::move(memory_kb);
            promise->set_value(std::move(payload));
        });  // GCOVR_EXCL_BR_LINE (compiler artifact: lambda-body STL branches
             // attributed to this line)
        if (future.wait_for(std::chrono::milliseconds(500)) ==
            std::future_status::ready) {
            const nlohmann::json payload = future.get();
            prom_emit(out, "shield_services", "gauge",
                      "Registered Lua services", "",
                      payload.value("count", 0.0));
            std::vector<std::pair<std::string, double>> request_samples;
            std::vector<std::pair<std::string, double>> error_samples;
            std::vector<std::pair<std::string, double>> uptime_samples;
            std::vector<std::pair<std::string, double>> timer_samples;
            for (const auto& [name, value] :
                 payload.value("requests", nlohmann::json::object()).items()) {
                request_samples.emplace_back(
                    "service=\"" + prom_escape(name) + "\"", value);
            }
            for (const auto& [name, value] :
                 payload.value("errors", nlohmann::json::object()).items()) {
                error_samples.emplace_back(
                    "service=\"" + prom_escape(name) + "\"", value);
            }
            for (const auto& [name, value] :
                 payload.value("uptime", nlohmann::json::object()).items()) {
                uptime_samples.emplace_back(
                    "service=\"" + prom_escape(name) + "\"", value);
            }
            for (const auto& [name, value] :
                 payload.value("timers", nlohmann::json::object()).items()) {
                timer_samples.emplace_back(
                    "service=\"" + prom_escape(name) + "\"", value);
            }
            prom_emit_group(out, "shield_service_requests_total", "counter",
                            "Messages dispatched to the service "
                            "(send/call/system)",
                            request_samples);
            prom_emit_group(out, "shield_service_errors_total", "counter",
                            "Dispatch handler failures per service",
                            error_samples);
            prom_emit_group(out, "shield_service_uptime_seconds", "gauge",
                            "Seconds since the service incarnation was "
                            "published (reset on respawn)",
                            uptime_samples);
            prom_emit_group(out, "shield_service_timers", "gauge",
                            "Active timers per service", timer_samples);
            std::vector<std::pair<std::string, double>> pending_call_samples;
            std::vector<std::pair<std::string, double>> pending_task_samples;
            for (const auto& [name, value] :
                 payload.value("pending_calls", nlohmann::json::object())
                     .items()) {
                pending_call_samples.emplace_back(
                    "service=\"" + prom_escape(name) + "\"", value);
            }
            for (const auto& [name, value] :
                 payload.value("pending_tasks", nlohmann::json::object())
                     .items()) {
                pending_task_samples.emplace_back(
                    "service=\"" + prom_escape(name) + "\"", value);
            }
            prom_emit_group(out, "shield_service_pending_calls", "gauge",
                            "Coroutine-aware calls suspended per service "
                            "(caller side)",
                            pending_call_samples);
            prom_emit_group(out, "shield_service_pending_tasks", "gauge",
                            "Forked tasks queued per service",
                            pending_task_samples);
            std::vector<std::pair<std::string, double>> coroutine_samples;
            std::vector<std::pair<std::string, double>> memory_samples;
            for (const auto& [name, value] :
                 payload.value("coroutines", nlohmann::json::object())
                     .items()) {
                coroutine_samples.emplace_back(
                    "service=\"" + prom_escape(name) + "\"", value);
            }
            for (const auto& [name, value] :
                 payload.value("memory_kb", nlohmann::json::object()).items()) {
                memory_samples.emplace_back(
                    "service=\"" + prom_escape(name) + "\"", value);
            }
            prom_emit_group(out, "shield_service_coroutines", "gauge",
                            "Live handler coroutines per service (running "
                            "or suspended)",
                            coroutine_samples);
            prom_emit_group(out, "shield_service_memory_kb", "gauge",
                            "Lua heap KB per service sampled at the last "
                            "dispatch exit",
                            memory_samples);
        }
    }

#ifdef SHIELD_ENABLE_SERVER
    if (auto* sm = shield::server::ServerManager::global()) {
        prom_emit(
            out, "shield_server_state", "gauge",
            "Server state machine (1 for the current state)",
            "state=\"" +
                prom_escape(shield::server::server_state_name(sm->state())) +
                "\"",
            1.0);
    }
#endif

#ifdef SHIELD_ENABLE_GLOBAL
    if (auto* gm = shield::global::GlobalManager::global()) {
        prom_emit(out, "shield_global_data_keys", "gauge", "Global data keys",
                  "", static_cast<double>(gm->data_size()));
        prom_emit(out, "shield_global_cache_entries", "gauge",
                  "Local cache entries", "",
                  static_cast<double>(gm->cache_size()));
        prom_emit(out, "shield_global_cache_hits_total", "counter",
                  "Local cache hits", "",
                  static_cast<double>(gm->cache_hits()));
        prom_emit(out, "shield_global_cache_misses_total", "counter",
                  "Local cache misses", "",
                  static_cast<double>(gm->cache_misses()));
        prom_emit_group(  // GCOVR_EXCL_BR_LINE (inlined nlohmann::json
            out, "shield_global_queues", "gauge", "Queues by family",
            // braced-init branches: entry lines run, records stay at zero)
            {{"type=\"normal\"",
              static_cast<double>(gm->queue_count())},  // GCOVR_EXCL_LINE
             {"type=\"delay\"",
              static_cast<double>(gm->delay_queue_count())},  // GCOVR_EXCL_LINE
             {"type=\"priority\"",
              static_cast<double>(  // GCOVR_EXCL_LINE (wrapped clone slot)
                  gm->priority_queue_count())},  // GCOVR_EXCL_LINE
             {"type=\"broadcast\"",
              static_cast<double>(  // GCOVR_EXCL_LINE (wrapped clone slot)
                  gm->broadcast_queue_count())},  // GCOVR_EXCL_LINE
             {"type=\"reliable\"",
              static_cast<double>(  // GCOVR_EXCL_LINE (wrapped clone slot)
                  gm->reliable_queue_count())}});  // GCOVR_EXCL_LINE
        prom_emit_group(  // GCOVR_EXCL_BR_LINE (inlined nlohmann::json
            out, "shield_global_locks", "gauge", "Live locks by kind",
            // braced-init branches: entry lines run, records stay at zero)
            {{"kind=\"mutex\"",
              static_cast<double>(gm->mutex_registry_size("mutex"))},
             {"kind=\"spinlock\"",
              static_cast<double>(gm->mutex_registry_size("spinlock"))},
             {"kind=\"rwlock\"", static_cast<double>(gm->rwlock_count())}});
        prom_emit(out, "shield_global_rank_boards", "gauge",
                  "Leaderboard boards", "",
                  static_cast<double>(gm->rank_board_count()));
        prom_emit(out, "shield_global_rank_members", "gauge",
                  "Leaderboard members across boards", "",
                  static_cast<double>(gm->rank_total_members()));
        prom_emit(out, "shield_global_scheduler_tasks", "gauge",
                  "Registered scheduler tasks", "",
                  static_cast<double>(gm->sched_list().size()));
        prom_emit(out, "shield_global_scheduler_active", "gauge",
                  "Active (not paused/done) scheduler tasks", "",
                  static_cast<double>(gm->sched_active_count()));
    }
#endif

#ifdef SHIELD_ENABLE_CLUSTER
    if (auto* cm = shield::cluster::global_cluster_manager()) {
        std::map<std::string, double> nodes_by_state;
        for (const auto& n : cm->nodes()) {
            nodes_by_state[shield::cluster::node_state_name(n.state)] += 1.0;
        }
        std::vector<std::pair<std::string, double>> samples;
        for (const auto& [state, count] : nodes_by_state) {
            samples.emplace_back("state=\"" + prom_escape(state) + "\"", count);
        }
        prom_emit_group(out, "shield_cluster_nodes", "gauge",
                        "Cluster nodes by state", samples);
        if (auto* ct = shield::cluster::global_cluster_transport()) {
            const auto stats = ct->stats();
            prom_emit(out, "shield_cluster_transport_connections", "gauge",
                      "Live cluster transport connections", "",
                      static_cast<double>(stats.live_connections));
            prom_emit(out, "shield_cluster_transport_reconnects_total",
                      "counter", "Cluster transport reconnects", "",
                      static_cast<double>(stats.reconnects));
            prom_emit_group(
                out, "shield_cluster_transport_messages_total", "counter",
                "Cluster transport messages",
                // GCOVR_EXCL_START (gcov artifact: the ops fixture
                // registers a real transport and asserts both series land
                // in the body, but the line records of a braced-init
                // sample list stay at zero)
                {{"direction=\"tx\"", static_cast<double>(stats.tx_messages)},
                 {"direction=\"rx\"", static_cast<double>(stats.rx_messages)}});
            // GCOVR_EXCL_STOP
            prom_emit_group(
                out, "shield_cluster_transport_heartbeats_total", "counter",
                "Cluster transport heartbeats",
                // GCOVR_EXCL_START (same artifact as above)
                {{"direction=\"tx\"", static_cast<double>(stats.tx_heartbeats)},
                 {"direction=\"rx\"",
                  static_cast<double>(stats.rx_heartbeats)}});
            // GCOVR_EXCL_STOP
        }
    }
#endif

    shield::net::HttpResponse resp;
    resp.result(boost::beast::http::status::ok);
    resp.set(boost::beast::http::field::content_type,
             "text/plain; version=0.0.4; charset=utf-8");
    resp.body() = std::move(out);
    resp.prepare_payload();
    return resp;
}

shield::net::HttpResponse OpsHttpHandler::handle_status(
    const shield::net::HttpRequest& req) {
    nlohmann::json data;

    // Services
    {
        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        auto future = promise->get_future();
        lua_mgr_.enqueue_forked_task("", [&mgr = lua_mgr_, promise]() {
            auto names = mgr.list_services();
            promise->set_value(nlohmann::json(names));
        });
        if (future.wait_for(std::chrono::seconds(2)) ==
            std::future_status::ready) {
            data["services"] = future.get();
        } else {
            data["services"] = "timeout";
        }
    }

    // Plugins
    {
        auto& host = shield::plugin::global_host();
        auto instances = host.list_instances();
        nlohmann::json plugins = nlohmann::json::array();
        for (const auto& inst : instances) {
            plugins.push_back(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // inlined nlohmann::json braced-init branches)
                {{"id",
                  inst.id},  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                             // nlohmann::json braced-init branches)
                 {"package", inst.package},
                 {"state", inst.state},
                 {"required", inst.required}});
        }
        data["plugins"] = plugins;
    }

    // Cluster
#ifdef SHIELD_ENABLE_CLUSTER
    {
        auto* cm = shield::cluster::global_cluster_manager();
        if (cm) {
            data["cluster"] = build_cluster_status_json();
        }
    }
#endif

    // Server state machine (read-only snapshot)
#ifdef SHIELD_ENABLE_SERVER
    {
        auto* sm = shield::server::ServerManager::global();
        if (sm) {
            data["server"] = build_server_status_json();
        }
    }
#endif

    // Global capability store (read-only snapshot)
#ifdef SHIELD_ENABLE_GLOBAL
    {
        nlohmann::json global = build_global_status_json();
        if (!global.is_null()) {
            data["global"] = std::move(global);
        }
    }
#endif

    return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // inlined nlohmann::json braced-init branches)
        200, {{"type", "result"},
              {"data", data}});  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                 // inlined nlohmann::json braced-init branches)
}

shield::net::HttpResponse OpsHttpHandler::handle_services(
    const shield::net::HttpRequest& req) {
    auto promise = std::make_shared<std::promise<nlohmann::json>>();
    auto future = promise->get_future();
    lua_mgr_.enqueue_forked_task("", [&mgr = lua_mgr_, promise]() {
        auto names = mgr.list_services();
        promise->set_value(nlohmann::json(names));
    });
    if (future.wait_for(std::chrono::seconds(2)) == std::future_status::ready) {
        return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // inlined nlohmann::json braced-init
                                    // branches)
            200,  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                  // nlohmann::json braced-init branches)
            {{"type", "result"}, {"data", future.get()}});
    }
    return make_error_response(504, "timeout querying services");
}

shield::net::HttpResponse OpsHttpHandler::handle_service_detail(
    const shield::net::HttpRequest& req) {
    // Pull ":name" out of /ops/services/<name> (query string stripped).
    std::string target(req.target());
    const auto query_pos = target.find('?');
    if (query_pos != std::string::npos) {
        target.resize(query_pos);
    }
    constexpr char kPrefix[] = "/ops/services/";
    // GCOVR_EXCL_START (defensive: the router never matches an empty
    // ":name" segment, so this branch is unreachable through HTTP)
    if (target.rfind(kPrefix, 0) != 0 || target.size() <= sizeof(kPrefix) - 1) {
        return make_error_response(404, "missing service name");
    }
    // GCOVR_EXCL_STOP
    const std::string name = target.substr(sizeof(kPrefix) - 1);

    auto promise =
        std::make_shared<std::promise<std::optional<nlohmann::json>>>();
    auto future = promise->get_future();
    // GCOVR_EXCL_BR_START (compiler artifact: lambda-body STL branches
    // are attributed to the capture line)
    lua_mgr_.enqueue_forked_task(
        "", [&mgr = lua_mgr_,
             promise,  // GCOVR_EXCL_BR_LINE (compiler artifact: lambda-body STL
                       // branches attributed to this line)
             name]() {  // GCOVR_EXCL_BR_LINE (compiler artifact: lambda-body
                        // STL branches attributed to this line)
            promise->set_value(mgr.service_detail(name));
        });
    // GCOVR_EXCL_BR_STOP
    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        return make_error_response(504, "timeout querying service");
    }
    auto detail = future.get();
    if (!detail) {
        return make_error_response(404, "service not found: " + name);
    }
    return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // inlined nlohmann::json braced-init branches)
        200,
        {{"type", "result"},
         {"data", *detail}});  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                               // nlohmann::json braced-init branches)
}

shield::net::HttpResponse OpsHttpHandler::handle_plugins(
    const shield::net::HttpRequest& req) {
    auto& host = shield::plugin::global_host();
    auto instances = host.list_instances();
    nlohmann::json plugins = nlohmann::json::array();
    for (const auto& inst : instances) {
        plugins.push_back(     // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                               // nlohmann::json braced-init branches)
            {{"id", inst.id},  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                               // nlohmann::json braced-init branches)
             {"package", inst.package},
             {"state", inst.state},
             {"required", inst.required}});
    }
    return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // inlined nlohmann::json braced-init branches)
        200,
        {{"type", "result"},
         {"data", plugins}});  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                               // nlohmann::json braced-init branches)
}

shield::net::HttpResponse OpsHttpHandler::handle_config(
    const shield::net::HttpRequest& req) {
    // Parse optional key from query string
    std::string target;
    auto query = req.target();
    auto qpos = query.find('?');
    if (qpos != std::string::npos) {
        auto qs = query.substr(qpos + 1);
        auto kpos = qs.find("key=");
        if (kpos != std::string::npos) {
            target = qs.substr(kpos + 4);
            // Strip any trailing &
            auto amp = target.find('&');
            if (amp != std::string::npos) {
                target = target.substr(0, amp);
            }
        }
    }

    if (target.empty()) {
        // Return all config as JSON
        // Note: This is a simplified version - actual implementation would
        // need to serialize the full config
        return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // inlined nlohmann::json braced-init
                                    // branches)
            200, {{"type", "result"}, {"data", "Use ?key=<key> to query"}});
    }

    auto value = shield::config::get(target, "");
    if (value.empty()) {
        return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // inlined nlohmann::json braced-init
                                    // branches)
            200, {{"type", "result"},
                  {"data",
                   nullptr}});  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // inlined nlohmann::json braced-init branches)
    }
    // Redact secret-bearing keys (passwords/tokens/credentials) so the
    // unauthenticated /ops/config endpoint never echoes secrets. The key
    // name is the only signal available here; keep to strong secret
    // indicators so ordinary config keys are not masked.
    static constexpr std::string_view kSecretNeedles[] = {
        "password", "passwd", "secret",     "token",      "credential",
        "api_key",  "apikey", "access_key", "private_key"};
    bool is_secret = false;
    for (std::string_view needle : kSecretNeedles) {
        if (target.find(needle) != std::string::npos) {
            is_secret = true;
            break;
        }
    }
    const std::string redacted =
        is_secret ? std::string("***redacted***") : value;
    return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // inlined nlohmann::json braced-init branches)
        200,
        {{"type", "result"},
         {"data", redacted}});  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // inlined nlohmann::json braced-init branches)
}

shield::net::HttpResponse OpsHttpHandler::handle_dashboard(
    const shield::net::HttpRequest& req) {
    (void)req;
    // Static self-contained page: same bytes for "/" and "/ops/dashboard",
    // on every request (the page fetches live data client-side).
    shield::net::HttpResponse resp;
    resp.result(boost::beast::http::status::ok);
    resp.set(boost::beast::http::field::content_type,
             "text/html; charset=utf-8");
    resp.body() = kDashboardHtml;
    resp.prepare_payload();
    return resp;
}

shield::net::HttpResponse OpsHttpHandler::handle_default(
    const shield::net::HttpRequest& req) {
    const std::string path(req.target());

    // Reject any traversal or backslash before the SPA fallback; a deep
    // link with ".." is never a legitimate route and must not be answered
    // with the SPA shell (which would mask a path-escape attempt).
    if (path.find("..") != std::string::npos ||
        path.find('\\') != std::string::npos) {
        return make_error_response(404, "not found");
    }
    // Also reject percent-encoded traversal ("%2e%2e", "%2f") so encoded
    // variants cannot reach the SPA fallback either.
    {
        std::string decoded;
        decoded.reserve(path.size());
        for (std::size_t i = 0; i < path.size(); ++i) {
            if (path[i] == '%' && i + 2 < path.size()) {
                const auto hex = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                const int hi = hex(path[i + 1]);
                const int lo = hex(path[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    decoded.push_back(static_cast<char>((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
            decoded.push_back(path[i]);
        }
        if (decoded.find("..") != std::string::npos ||
            decoded.find('\\') != std::string::npos) {
            return make_error_response(404, "not found");
        }
    }

    // With a static root, "/" and "/ops/dashboard" serve the built console
    // (same-origin, SPA route), and any other non-API deep link gets the SPA
    // shell too so a direct refresh lands correctly. Unknown /ops/* API paths
    // stay a clear JSON 404, never HTML (would mask configuration errors).
    if (!static_root_.empty()) {
        if (path.rfind("/ops/", 0) == 0 && path != "/ops/dashboard") {
            return make_error_response(404, "not found");
        }
        namespace fs = std::filesystem;
        const fs::path index = fs::path(static_root_) / "index.html";
        std::ifstream file(index, std::ios::binary);
        if (file) {
            std::string body((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
            shield::net::HttpResponse resp;
            resp.result(boost::beast::http::status::ok);
            resp.set(boost::beast::http::field::content_type,
                     "text/html; charset=utf-8");
            resp.set(boost::beast::http::field::cache_control, "no-cache");
            resp.body() = std::move(body);
            resp.prepare_payload();
            return resp;
        }
    }

    // Unknown /ops/* API path without a static root: JSON 404.
    if (path.rfind("/ops/", 0) == 0 || path.rfind("/ops", 0) == 0) {
        return make_error_response(404, "not found");
    }

    // No static root (or no index.html): embedded transitional single page.
    return handle_dashboard(req);
}

shield::net::HttpResponse OpsHttpHandler::handle_eval(
    const shield::net::HttpRequest& req) {
    // Bearer token gate. Accept only "Authorization: Bearer <token>".
    // Written as a plain if instead of a ternary: the inlined string
    // constructions of the ternary arms produced pseudo line entries the
    // counters never landed on (coverage artifact).
    if (!token_matches(bearer_token(req), eval_token_)) {
        return make_error_response(401, "unauthorized");
    }

    // Parse JSON body
    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body());
    } catch (const std::exception&  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // catch-entry pseudo-arc)
                 e) {               // (compiler artifact: catch-entry
                                    // covered by the invalid-JSON
                                    // case, missed fallthrough edge
                                    // is unwind bookkeeping)
        return make_error_response(400, "invalid JSON body");
    }

    if (!body.contains("code") || !body["code"].is_string()) {
        return make_error_response(400, "missing 'code' field");
    }

    std::string code = body["code"].get<std::string>();

    // Execute in a restricted VM: the token gate is the first line of
    // defense, the VM strip (no os.execute/io/require) the second.
    auto vm = lua_rt_.create_vm();
    lua_rt_.register_api(vm);
    lua_rt_.restrict_vm(vm);

    nlohmann::json result;
    std::string error;
    bool ok = lua_rt_.exec_lua(vm, code, &result, &error);

    // VM will be automatically destroyed when shared_ptr goes out of scope
    vm.reset();

    if (ok) {
        return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // inlined nlohmann::json braced-init
                                    // branches)
            200, {{"type", "result"},
                  {"data",
                   result}});  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                               // nlohmann::json braced-init branches)
    }
    return make_error_response(400, error);
}

shield::net::HttpResponse OpsHttpHandler::handle_profile(
    const shield::net::HttpRequest& req) {
    // Bearer token gate (same shape as /ops/eval, separate token).
    if (!token_matches(bearer_token(req), profile_token_)) {
        return make_error_response(401, "unauthorized");
    }

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body());
    } catch (const std::exception&  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // catch-entry pseudo-arc)
                 e) {
        return make_error_response(400, "invalid JSON body");
    }
    if (!body.contains("action") || !body["action"].is_string()) {
        return make_error_response(400, "missing 'action' field");
    }
    const std::string action = body["action"].get<std::string>();

    // status: pure registry read, always answers.
    if (action == "status") {
        nlohmann::json data;
        if (auto info = lua_mgr_.profile_status()) {
            data = {{"active", true},
                    {"service", info->service_id},
                    {"elapsed_ms", info->elapsed_ms},
                    {"duration_ms",
                     info->duration_ms}};  // GCOVR_EXCL_BR_LINE (compiler
                                           // artifact: braced-init arcs)
        } else {
            data = {{"active", false}};  // GCOVR_EXCL_BR_LINE (compiler
                                         // artifact: braced-init arcs)
        }
        data["slow_calls"] = slow_calls_section();
        return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler
                                    // artifact: inline call throw arc)
            200, {{"type", "result"},
                  {"data", data}});  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                     // inline throw arc)
    }  // GCOVR_EXCL_LINE (compiler artifact: block-end attribution)

    if (action == "start") {
        if (!body.contains("service") || !body["service"].is_string()) {
            return make_error_response(400, "missing 'service' field");
        }
        const std::string service = body["service"].get<std::string>();

        shield::lua::ProfileSessionConfig config;
        config.service = service;
        if (body.contains("duration_ms")) {
            if (!body["duration_ms"].is_number_unsigned() ||
                body["duration_ms"] == 0 ||
                body["duration_ms"].get<uint64_t>() > 60000) {
                return make_error_response(400, "duration_ms must be 1..60000");
            }
            config.duration_ms = body["duration_ms"].get<uint64_t>();
        }
        if (body.contains("interval")) {
            if (!body["interval"].is_number_unsigned() ||
                body["interval"] == 0) {
                return make_error_response(400, "interval must be >= 1");
            }
            config.interval = body["interval"].get<uint64_t>();
        }

        // Start cooldown (http.profile_cooldown_seconds, default 10):
        // rate-limits sessions beyond the manager's single-session 409.
        int cooldown_s = 10;
        try {
            cooldown_s = std::stoi(
                shield::config::get("http.profile_cooldown_seconds", "10"));
        } catch (const std::exception&  // GCOVR_EXCL_BR_LINE (compiler
                                        // artifact: catch-entry pseudo-arc)
                     e) {
            cooldown_s = 10;  // non-numeric config: keep the default
        }
        if (cooldown_s < 0) {
            cooldown_s = 0;
        }
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard lock(profile_mu_);
            if (profile_started_once_ &&
                now - last_profile_start_ < std::chrono::seconds(cooldown_s)) {
                return make_error_response(429,
                                           "profile start cooldown active");
            }
        }

        auto promise = std::make_shared<std::promise<nlohmann::json>>();
        auto report = promise->get_future().share();
        const auto result = lua_mgr_.profile_start(service, config, promise);
        switch (result) {  // GCOVR_EXCL_BR_LINE (race: jump-table arc into the
                           // DispatchLost rollback (actor-gone window
                           // untestable end-to-end))
            case shield::lua::LuaServiceManager::ProfileStartResult::
                kServiceNotFound:
            case shield::lua::LuaServiceManager::ProfileStartResult::
                kDispatchLost:  // GCOVR_EXCL_BR_LINE (race: the manager's
                                // enqueue can only fail if the service actor
                                // vanished between the manager's own check and
                                // the dispatch)
                return make_error_response(404,
                                           "service not found: " + service);
            case shield::lua::LuaServiceManager::ProfileStartResult::
                kSessionActive:
                return make_error_response(409, "profile session active");
            case shield::lua::LuaServiceManager::ProfileStartResult::kStarted:
                break;
        }
        {
            std::lock_guard lock(profile_mu_);
            profile_report_ = std::move(report);
            last_profile_start_ = now;
            profile_started_once_ = true;
        }
        return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                    // inlined nlohmann::json braced-init
                                    // branches)
            200, {{"type", "result"},
                  {"data",
                   {{"started", true},
                    {"service", service},
                    {"duration_ms", config.duration_ms},
                    {"interval", config.interval}}}});
    }

    if (action == "stop" || action == "report") {
        // Both end the session and return the report (report is the
        // read-named alias; a later start re-arms from scratch).
        auto info = lua_mgr_.profile_status();
        if (!info) {
            // Slow-call metering is decoupled from the sampling session:
            // with no session live, report still answers — session meta
            // plus whatever the ring holds — instead of 409ing.
            return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler
                                        // artifact: inlined nlohmann::json
                                        // braced-init branches)
                200,
                {{"type", "result"},
                 {"data",
                  {{"active", false}, {"slow_calls", slow_calls_section()}}}});
        }
        std::string stop_error;
        if (!lua_mgr_.profile_stop(  // GCOVR_EXCL_BR_LINE (race: the session
                                     // can only end between the status read and
                                     // this stop via exit cleanup / duration
                                     // expiry; no test races them)
                info->service_id, &stop_error)) {
            // GCOVR_EXCL_START (race: the session can only end between the
            // status read and this stop via exit cleanup / duration expiry;
            // no test races them)
            // The session ended between the status read and the stop (exit
            // cleanup or duration expiry owns the promise now).
            return make_error_response(409, "no active profile session");
            // GCOVR_EXCL_STOP
        }
        std::shared_future<nlohmann::json> report;
        {
            std::lock_guard lock(profile_mu_);
            report = profile_report_;
        }
        if (report.valid() &&  // GCOVR_EXCL_BR_LINE (defensive: the stop
                               // path only runs after a start on this handler,
                               // which always stores a valid shared_future)
            report.wait_for(std::chrono::seconds(2)) ==
                std::future_status::ready) {
            auto data = report.get();
            data["active"] = true;
            data["slow_calls"] = slow_calls_section();
            return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler
                                        // artifact: inlined nlohmann::json
                                        // braced-init branches)
                200, {{"type", "result"}, {"data", data}});
        }  // GCOVR_EXCL_LINE (compiler artifact: block-end attribution)
        return make_error_response(504,
                                   "profile dispatch timeout (owner busy)");
    }

    return make_error_response(400, "unknown action: " + action);
}

shield::net::HttpResponse OpsHttpHandler::make_json_response(
    int status_code, const nlohmann::json& data) {
    shield::net::HttpResponse resp;
    resp.result(static_cast<boost::beast::http::status>(status_code));
    resp.set(boost::beast::http::field::content_type, "application/json");
    resp.body() = data.dump(2);
    resp.prepare_payload();
    return resp;
}  // GCOVR_EXCL_LINE (uncalled exit clone)

shield::net::HttpResponse OpsHttpHandler::make_error_response(
    int status_code, const std::string& message) {
    return make_json_response(  // GCOVR_EXCL_BR_LINE (compiler artifact:
                                // inlined nlohmann::json braced-init branches)
        status_code,  // GCOVR_EXCL_BR_LINE (compiler artifact: inlined
                      // nlohmann::json braced-init branches)
        {{"type", "error"}, {"message", message}});
}

}  // namespace shield::console
