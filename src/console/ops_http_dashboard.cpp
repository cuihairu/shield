// [SHIELD_CONSOLE] The embedded ops dashboard (self-contained HTML
// raw string) and its serving endpoint.
// Split out of ops_http_handler.cpp.
#include "shield/console/ops_http_handler.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <psapi.h>
#include <tlhelp32.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#endif

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

}  // namespace shield::console
