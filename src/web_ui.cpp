#include "web_ui.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>

#ifdef __linux__
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

static std::string jsonEscape(const std::string& s) {
    std::ostringstream out;
    for (unsigned char ch : s) {
        switch (ch) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(ch) << std::dec;
                } else {
                    out << static_cast<char>(ch);
                }
        }
    }
    return out.str();
}

static std::string layoutJson(const UiLayout& l) {
    std::ostringstream out;
    out << "{\"rows\":" << l.rows
        << ",\"cols\":" << l.cols
        << ",\"flag\":{\"r\":" << l.flagR << ",\"c\":" << l.flagC << "}"
        << ",\"walls\":[";
    for (size_t i = 0; i < l.walls.size(); ++i) {
        if (i) out << ',';
        out << "[" << l.walls[i].r << ',' << l.walls[i].c << "]";
    }
    out << "]}";
    return out.str();
}

static void writeAgentJson(std::ostringstream& out, const UiAgentView& a) {
    out << "{\"id\":" << a.id
        << ",\"role\":" << a.role
        << ",\"name\":\"" << jsonEscape(a.name) << "\""
        << ",\"r\":" << a.r
        << ",\"c\":" << a.c
        << ",\"steps\":" << a.steps
        << ",\"frozen\":" << (a.frozen ? "true" : "false")
        << ",\"canShoot\":" << (a.canShoot ? "true" : "false")
        << ",\"freezeRemainingMs\":" << a.freezeRemainingMs
        << ",\"cooldownRemainingMs\":" << a.cooldownRemainingMs
        << '}';
}

static std::string stateJson(const UiState& s) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "{\"rows\":" << s.rows
        << ",\"cols\":" << s.cols
        << ",\"totalAgents\":" << s.totalAgents
        << ",\"displayedAgents\":" << s.displayedAgents
        << ",\"totalSteps\":" << s.totalSteps
        << ",\"frozenAgents\":" << s.frozenAgents
        << ",\"delayMs\":" << s.delayMs
        << ",\"winner\":" << s.winner
        << ",\"paused\":" << (s.paused ? "true" : "false")
        << ",\"gameOver\":" << (s.gameOver ? "true" : "false")
        << ",\"elapsedSec\":" << s.elapsedSec
        << ",\"agents\":[";
    for (size_t i = 0; i < s.agents.size(); ++i) {
        if (i) out << ',';
        writeAgentJson(out, s.agents[i]);
    }
    out << "]";
    out << ",\"selected\":";
    if (s.hasSelected) writeAgentJson(out, s.selected);
    else out << "null";
    out << '}';
    return out.str();
}

static std::map<std::string, std::string> parseQuery(const std::string& query) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        std::string part = query.substr(pos, amp - pos);
        size_t eq = part.find('=');
        if (eq == std::string::npos) out[part] = "";
        else out[part.substr(0, eq)] = part.substr(eq + 1);
        pos = amp + 1;
    }
    return out;
}

static int intParam(const std::map<std::string, std::string>& q,
                    const std::string& key,
                    int fallback) {
    auto it = q.find(key);
    if (it == q.end()) return fallback;
    try { return std::stoi(it->second); }
    catch (...) { return fallback; }
}

static std::string httpResponse(const std::string& body,
                                const std::string& contentType = "application/json; charset=utf-8",
                                const std::string& status = "200 OK") {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << "\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Cache-Control: no-store\r\n"
        << "Connection: close\r\n"
        << "X-Content-Type-Options: nosniff\r\n"
        << "\r\n"
        << body;
    return out.str();
}

static const char* kDashboardHtml = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FlagChase — Concurrent Simulation</title>
<style>
:root{
  --bg:#081019;--panel:#0e1925;--panel2:#111f2d;--line:#26394b;--line2:#35516b;
  --text:#e8eef5;--muted:#8fa3b5;--green:#35d07f;--blue:#48a9ff;--red:#ff6257;
  --orange:#ff9e42;--yellow:#ffd65a;--purple:#b58cff;--wall:#657486;--cell:#07111a;
}
*{box-sizing:border-box}
html,body{height:100%}
body{margin:0;background:linear-gradient(180deg,#071019,#050a10);color:var(--text);font-family:Inter,ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;font-size:13px;overflow:hidden}
button,input{font:inherit}
button{cursor:pointer}
.topbar{height:54px;display:flex;align-items:center;gap:20px;padding:0 18px;border-bottom:1px solid #203244;background:#08131e}
.brand{font-weight:800;font-size:17px;letter-spacing:.01em}.brand span{color:var(--blue)}
.topmetrics{margin-left:auto;display:flex;gap:24px;align-items:center;color:#c3cfdb}.topmetrics b{color:#fff;font-variant-numeric:tabular-nums}
.run{font-weight:700}.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--green);margin-right:7px}.run.paused{color:var(--yellow)}.run.paused .dot{background:var(--yellow)}.run.stopped{color:var(--orange)}.run.stopped .dot{background:var(--orange)}
.app{height:calc(100% - 54px);padding:10px;display:grid;grid-template-columns:minmax(0,1fr) 330px;grid-template-rows:minmax(0,1fr) 188px;gap:10px}
.panel{background:linear-gradient(180deg,var(--panel),#0b1520);border:1px solid var(--line);border-radius:8px;box-shadow:0 10px 30px rgba(0,0,0,.18);overflow:hidden}
.boardpanel{display:flex;flex-direction:column;min-width:0;min-height:0}
.boardhead{height:44px;display:flex;align-items:center;padding:0 12px;border-bottom:1px solid var(--line);background:#0d1925;gap:14px}.boardhead strong{font-size:14px}.boardhead .sub{color:var(--muted);font-size:11px}.boardtools{margin-left:auto;display:flex;gap:13px;align-items:center}.toggle{display:flex;gap:6px;align-items:center;color:#aebdca;font-size:11px;white-space:nowrap}.toggle input{accent-color:var(--blue)}
.canvaswrap{position:relative;flex:1;min-height:0;background:#040a10;overflow:hidden}.canvaswrap canvas{display:block;width:100%;height:100%;cursor:crosshair}
.hint{position:absolute;left:10px;bottom:10px;padding:6px 9px;border:1px solid #314a60;border-radius:5px;background:rgba(6,13,21,.88);color:#91a7ba;font-size:10px;pointer-events:none}.legend{position:absolute;right:10px;bottom:10px;display:flex;gap:12px;padding:6px 9px;border:1px solid #314a60;border-radius:5px;background:rgba(6,13,21,.88);font-size:10px;color:#b8c5d0;pointer-events:none}.legend span{display:flex;align-items:center;gap:5px}.lg{width:9px;height:9px;display:inline-block}.lg.runner{border-radius:50%;background:var(--blue)}.lg.jumper{width:0;height:0;border-left:5px solid transparent;border-right:5px solid transparent;border-bottom:9px solid var(--red)}.lg.controller{background:var(--orange)}.lg.wall{background:var(--wall)}.lg.flag{color:var(--yellow);font-size:13px;width:auto;height:auto}
.sidebar{display:flex;flex-direction:column;gap:10px;min-height:0}.section{background:linear-gradient(180deg,var(--panel),#0b1520);border:1px solid var(--line);border-radius:8px;overflow:hidden}.section.grow{flex:1;min-height:0}.stitle{height:37px;display:flex;align-items:center;padding:0 11px;border-bottom:1px solid var(--line);font-weight:750;font-size:12px}.sbody{padding:11px}.controls{display:grid;grid-template-columns:1fr 1fr 1fr;gap:7px}.btn{border:1px solid #35506a;border-radius:5px;padding:8px 9px;background:#16283a;color:#eef5fb;font-weight:700;font-size:11px}.btn:hover{filter:brightness(1.12)}.btn.primary{background:#1979c7;border-color:#2587d8}.btn.good{background:#188455;border-color:#229563}.btn.stop{background:#963f3d;border-color:#a84d49}.btn.ghost{background:#101e2c}.row{display:flex;align-items:center;gap:8px;margin-top:10px}.row label{color:#aebdca;flex:1}.row .value{color:#fff;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-variant-numeric:tabular-nums}.slider{width:100%;accent-color:var(--blue)}.search{display:flex;gap:6px}.search input{min-width:0;flex:1;background:#0b1723;border:1px solid #35506a;border-radius:5px;color:#fff;padding:7px 8px}.search .btn{flex:0 0 auto}.small{font-size:10px;color:var(--muted);line-height:1.45}
.kv{display:grid;grid-template-columns:1fr auto;gap:7px 10px}.kv .k{color:#9cafbf}.kv .v{color:#fff;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;text-align:right;font-variant-numeric:tabular-nums}.badge{display:inline-block;border:1px solid #45627c;border-radius:999px;padding:2px 7px;font-size:10px;font-family:inherit}.badge.active{color:#65dfa0;border-color:#357659}.badge.frozen{color:#7cc9ff;border-color:#3d79a2}.agentactions{display:grid;grid-template-columns:1fr 1fr;gap:7px;margin-top:10px}.spatial{margin-top:10px;padding:9px;border:1px solid #263d52;border-radius:5px;background:#091520;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;color:#a9bed0;font-size:10px;line-height:1.6}.spatial strong{color:#fff}
.telemetry{grid-column:1 / span 2;display:grid;grid-template-columns:250px 1fr 1fr;gap:10px;min-height:0}.summary{padding:11px}.summary .title,.charttitle{font-weight:750;font-size:12px;margin-bottom:9px}.metricgrid{display:grid;grid-template-columns:1fr auto;gap:6px 10px;color:#aebdca}.metricgrid b{color:#fff;font-family:ui-monospace,SFMono-Regular,Menlo,monospace}.chartpanel{padding:10px;display:flex;flex-direction:column;min-width:0}.chartwrap{flex:1;min-height:0}.chartwrap canvas{width:100%;height:100%;display:block}.architecture{margin-top:8px;color:#7690a5;font-size:10px;line-height:1.45}
@media(max-width:1050px){body{overflow:auto}.app{height:auto;min-height:calc(100% - 54px);grid-template-columns:1fr;grid-template-rows:600px auto auto}.sidebar{grid-row:2}.telemetry{grid-column:1;grid-row:3;grid-template-columns:1fr}.topmetrics{gap:12px}.topmetrics span:nth-child(2),.topmetrics span:nth-child(3){display:none}}
</style>
</head>
<body>
<header class="topbar">
  <div class="brand"><span>Flag</span>Chase — Concurrent 2D Simulation</div>
  <div class="topmetrics">
    <span id="runState" class="run"><i class="dot"></i>Running</span>
    <span>Steps <b id="topSteps">0</b></span>
    <span>Elapsed <b id="topTime">00:00</b></span>
    <span>Live TPS <b id="topTps">0</b></span>
  </div>
</header>
<main class="app">
  <section class="panel boardpanel">
    <div class="boardhead">
      <strong>Simulation Board</strong>
      <span id="boardSize" class="sub">—</span>
      <div class="boardtools">
        <label class="toggle"><input id="gridToggle" type="checkbox" checked> Grid</label>
        <label class="toggle"><input id="indexToggle" type="checkbox"> Spatial index</label>
        <label class="toggle"><input id="editWalls" type="checkbox"> Edit walls</label>
        <button id="resetView" class="btn ghost">Reset view</button>
      </div>
    </div>
    <div class="canvaswrap">
      <canvas id="sim"></canvas>
      <div class="hint">Click agent to inspect · wheel to zoom · Shift+drag to pan</div>
      <div class="legend"><span><i class="lg runner"></i>Runner</span><span><i class="lg jumper"></i>Jumper</span><span><i class="lg controller"></i>Controller</span><span><i class="lg wall"></i>Wall</span><span><i class="lg flag">★</i>Goal</span></div>
    </div>
  </section>

  <aside class="sidebar">
    <section class="section">
      <div class="stitle">Simulation Control</div>
      <div class="sbody">
        <div class="controls"><button id="pauseBtn" class="btn good">Pause</button><button id="resumeBtn" class="btn primary">Resume</button><button id="stopBtn" class="btn stop">Stop</button></div>
        <div class="row"><label>Frame delay</label><span class="value"><span id="delayValue">0</span> ms</span></div>
        <input id="delay" class="slider" type="range" min="0" max="500" step="5" value="0">
        <div class="small" style="margin-top:8px">UI controls are queued and applied by the simulation control loop; the browser never mutates shared board state directly.</div>
      </div>
    </section>

    <section class="section grow">
      <div class="stitle">Live Agent State</div>
      <div class="sbody">
        <div class="search"><input id="agentSearch" type="number" min="0" placeholder="Agent ID"><button id="selectAgentBtn" class="btn">Inspect</button></div>
        <div id="agentPanel" style="margin-top:11px"><div class="small">Click an agent on the board or enter an ID.</div></div>
        <div id="spatialPanel" class="spatial">Select an agent to inspect its direct cell index.</div>
      </div>
    </section>
  </aside>

  <section class="telemetry">
    <div class="panel summary">
      <div class="title">Engineering Telemetry</div>
      <div class="metricgrid">
        <span>Agents</span><b id="statAgents">0</b>
        <span>Rendered</span><b id="renderedCount">0</b>
        <span>Frozen</span><b id="statFrozen">0</b>
        <span>Board</span><b id="statBoard">—</b>
        <span>Total steps</span><b id="statSteps">0</b>
        <span>Live TPS</span><b id="statTps">0</b>
      </div>
      <div class="architecture">Thread-per-agent · mutex-protected shared state · O(1) direct cell occupancy index · snapshot-based browser reads</div>
    </div>
    <div class="panel chartpanel"><div class="charttitle">Rolling Throughput</div><div class="chartwrap"><canvas id="tpsChart"></canvas></div></div>
    <div class="panel chartpanel"><div class="charttitle">Frozen Agents</div><div class="chartwrap"><canvas id="frozenChart"></canvas></div></div>
  </section>
</main>
<script>
const $=id=>document.getElementById(id);
let layout=null,state=null,selected=-1,lastSteps=0,lastPoll=performance.now(),rollingTps=0;
let tpsHist=[],frozenHist=[],zoom=1,panX=0,panY=0,drag=false,dragStart=null,wallSet=new Set();
const roleColor=['#48a9ff','#ff6257','#ff9e42'];
const roleName=['Runner','Jumper','Controller'];

async function getJson(url,opt){const r=await fetch(url,opt);if(!r.ok)throw new Error(await r.text());return r.json()}
function fmtTime(sec){sec=Math.max(0,Math.floor(sec));const h=Math.floor(sec/3600),m=Math.floor((sec%3600)/60),s=sec%60;return h?`${String(h).padStart(2,'0')}:${String(m).padStart(2,'0')}:${String(s).padStart(2,'0')}`:`${String(m).padStart(2,'0')}:${String(s).padStart(2,'0')}`}
async function loadLayout(){layout=await getJson('/api/layout');wallSet=new Set(layout.walls.map(w=>w[0]+','+w[1]));$('boardSize').textContent=`${layout.rows} × ${layout.cols}`;$('statBoard').textContent=`${layout.rows}×${layout.cols}`;draw()}
async function command(cmd,args={}){const q=new URLSearchParams({cmd,...args});await getJson('/api/control?'+q.toString(),{method:'POST'});if(cmd==='addwall'||cmd==='removewall')await loadLayout()}

function updateStats(s){
  $('topSteps').textContent=s.totalSteps.toLocaleString();$('topTime').textContent=fmtTime(s.elapsedSec);
  $('statAgents').textContent=s.totalAgents.toLocaleString();$('statFrozen').textContent=s.frozenAgents.toLocaleString();$('statSteps').textContent=s.totalSteps.toLocaleString();
  $('renderedCount').textContent=`${s.displayedAgents.toLocaleString()} / ${s.totalAgents.toLocaleString()}`;$('delay').value=s.delayMs;$('delayValue').textContent=s.delayMs;
  const now=performance.now(),dt=(now-lastPoll)/1000,ds=s.totalSteps-lastSteps;
  if(lastSteps>0&&dt>0){const inst=ds/dt;rollingTps=rollingTps?rollingTps*.72+inst*.28:inst;tpsHist.push(rollingTps);frozenHist.push(s.frozenAgents);if(tpsHist.length>100)tpsHist.shift();if(frozenHist.length>100)frozenHist.shift()}
  lastSteps=s.totalSteps;lastPoll=now;$('topTps').textContent=Math.round(rollingTps).toLocaleString();$('statTps').textContent=Math.round(rollingTps).toLocaleString();
  const rs=$('runState');rs.className='run'+(s.gameOver?' stopped':s.paused?' paused':'');rs.innerHTML=`<i class="dot"></i>${s.gameOver?'Stopped':s.paused?'Paused':'Running'}`;$('stopBtn').textContent=s.gameOver?'Close':'Stop';drawCharts()
}
function updateAgent(a){
  const p=$('agentPanel'),sp=$('spatialPanel');
  if(!a){p.innerHTML='<div class="small">Click an agent on the board or enter an ID.</div>';sp.textContent='Select an agent to inspect its direct cell index.';return}
  const op=a.frozen?'FROZEN':'ACTIVE';const shoot=a.role===2?(a.canShoot?'READY':`${a.cooldownRemainingMs} ms`):'N/A';
  p.innerHTML=`<div class="kv"><span class="k">ID</span><span class="v">${a.id}</span><span class="k">Role</span><span class="v">${roleName[a.role]||'Agent'}</span><span class="k">State</span><span class="v"><span class="badge ${a.frozen?'frozen':'active'}">${op}</span></span><span class="k">Position</span><span class="v">(${a.r}, ${a.c})</span><span class="k">Steps</span><span class="v">${a.steps.toLocaleString()}</span><span class="k">Freeze left</span><span class="v">${a.freezeRemainingMs} ms</span><span class="k">Shoot capability</span><span class="v">${shoot}</span></div><div class="agentactions"><button class="btn primary" onclick="command('freeze',{id:${a.id},ms:1500})">Freeze 1.5s</button><button class="btn ghost" onclick="command('unfreeze',{id:${a.id}})">Unfreeze</button></div>`;
  const idx=a.r*state.cols+a.c;sp.innerHTML=`<strong>Spatial occupancy index</strong><br>cell = (${a.r}, ${a.c})<br>index = ${idx}<br>occupant[index] = ${a.id}<br><span class="small">Direct O(1) exact-cell lookup</span>`
}
async function poll(){
  try{state=await getJson('/api/state?selected='+selected);updateStats(state);updateAgent(state.selected);draw()}catch(e){console.error(e)}
  setTimeout(poll,100)
}
function resizeCanvas(c){const dpr=window.devicePixelRatio||1,r=c.getBoundingClientRect(),w=Math.max(1,Math.floor(r.width*dpr)),h=Math.max(1,Math.floor(r.height*dpr));if(c.width!==w||c.height!==h){c.width=w;c.height=h}return{w,h,dpr}}
function cellGeometry(){const c=$('sim'),s=resizeCanvas(c);return{...s,cw:s.w/Math.max(1,layout.cols),ch:s.h/Math.max(1,layout.rows),viewW:s.w,viewH:s.h}}
function draw(){
  if(!layout||!state)return;const c=$('sim'),ctx=c.getContext('2d'),g=cellGeometry();ctx.clearRect(0,0,g.viewW,g.viewH);ctx.save();ctx.translate(panX,panY);ctx.scale(zoom,zoom);
  ctx.fillStyle='#050c13';ctx.fillRect(0,0,g.viewW/zoom,g.viewH/zoom);
  if($('gridToggle').checked&&(g.cw*zoom>4&&g.ch*zoom>4)){ctx.strokeStyle='rgba(75,108,136,.20)';ctx.lineWidth=1/zoom;ctx.beginPath();for(let x=0;x<=layout.cols;x++){ctx.moveTo(x*g.cw,0);ctx.lineTo(x*g.cw,layout.rows*g.ch)}for(let y=0;y<=layout.rows;y++){ctx.moveTo(0,y*g.ch);ctx.lineTo(layout.cols*g.cw,y*g.ch)}ctx.stroke()}
  for(const [r,col] of layout.walls){ctx.fillStyle='#657486';ctx.fillRect(col*g.cw,r*g.ch,Math.ceil(g.cw),Math.ceil(g.ch))}
  if($('indexToggle').checked&&g.cw*zoom>30&&g.ch*zoom>22){ctx.fillStyle='rgba(128,163,191,.45)';ctx.font=`${Math.max(7,Math.min(10,g.ch*.22))}px ui-monospace,monospace`;ctx.textAlign='left';ctx.textBaseline='top';for(let r=0;r<layout.rows;r++)for(let col=0;col<layout.cols;col++)ctx.fillText(String(r*layout.cols+col),col*g.cw+2/zoom,r*g.ch+2/zoom)}
  const fr=layout.flag.r,fc=layout.flag.c;ctx.fillStyle='#ffd65a';ctx.font=`${Math.max(10,Math.min(g.cw,g.ch)*1.05)}px sans-serif`;ctx.textAlign='center';ctx.textBaseline='middle';ctx.fillText('★',(fc+.5)*g.cw,(fr+.5)*g.ch);
  for(const a of state.agents){const x=(a.c+.5)*g.cw,y=(a.r+.5)*g.ch,rad=Math.max(2,Math.min(g.cw,g.ch)*.34);ctx.fillStyle=roleColor[a.role]||'#fff';ctx.strokeStyle=a.id===selected?'#fff':'rgba(0,0,0,.35)';ctx.lineWidth=(a.id===selected?2.4:1)/zoom;ctx.beginPath();if(a.role===0){ctx.arc(x,y,rad,0,Math.PI*2)}else if(a.role===1){ctx.moveTo(x,y-rad);ctx.lineTo(x+rad,y+rad);ctx.lineTo(x-rad,y+rad);ctx.closePath()}else{ctx.rect(x-rad,y-rad,rad*2,rad*2)}ctx.fill();ctx.stroke();if(a.frozen&&rad>3){ctx.strokeStyle='#9bd7ff';ctx.lineWidth=2/zoom;ctx.beginPath();ctx.arc(x,y,rad*1.45,0,Math.PI*2);ctx.stroke()}}
  ctx.restore()
}
function canvasToCell(e){if(!layout)return null;const c=$('sim'),r=c.getBoundingClientRect(),dpr=window.devicePixelRatio||1,g=cellGeometry(),x=((e.clientX-r.left)*dpr-panX)/zoom,y=((e.clientY-r.top)*dpr-panY)/zoom,col=Math.floor(x/g.cw),row=Math.floor(y/g.ch);if(row<0||row>=layout.rows||col<0||col>=layout.cols)return null;return{r:row,c:col}}
function drawLineChart(canvas,data,color){const ctx=canvas.getContext('2d'),S=resizeCanvas(canvas),w=S.w,h=S.h;ctx.clearRect(0,0,w,h);ctx.strokeStyle='#23394c';ctx.lineWidth=1;for(let i=1;i<4;i++){const y=h*i/4;ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(w,y);ctx.stroke()}if(data.length<2)return;let max=Math.max(...data,1),min=Math.min(...data,0);if(max===min)max=min+1;ctx.strokeStyle=color;ctx.lineWidth=2*(S.dpr||1);ctx.beginPath();data.forEach((v,i)=>{const x=i/Math.max(1,data.length-1)*w,y=h-((v-min)/(max-min))*(h*.78)-h*.1;i?ctx.lineTo(x,y):ctx.moveTo(x,y)});ctx.stroke();ctx.fillStyle='#8fa3b5';ctx.font=`${10*(S.dpr||1)}px sans-serif`;ctx.fillText(Math.round(max).toLocaleString(),5,12*(S.dpr||1));ctx.fillText(Math.round(min).toLocaleString(),5,h-4)}
function drawCharts(){drawLineChart($('tpsChart'),tpsHist,'#35d07f');drawLineChart($('frozenChart'),frozenHist,'#b58cff')}

$('pauseBtn').onclick=()=>command('pause');$('resumeBtn').onclick=()=>command('resume');$('stopBtn').onclick=()=>command('stop');
$('delay').oninput=e=>$('delayValue').textContent=e.target.value;$('delay').onchange=e=>command('speed',{ms:e.target.value});
$('selectAgentBtn').onclick=()=>{selected=Math.max(-1,parseInt($('agentSearch').value||'-1'));};$('gridToggle').onchange=draw;$('indexToggle').onchange=draw;
$('resetView').onclick=()=>{zoom=1;panX=0;panY=0;draw()};
const sim=$('sim');
sim.addEventListener('wheel',e=>{e.preventDefault();const old=zoom;zoom=Math.max(.5,Math.min(12,zoom*(e.deltaY<0?1.15:.87)));const rect=sim.getBoundingClientRect(),dpr=window.devicePixelRatio||1,mx=(e.clientX-rect.left)*dpr,my=(e.clientY-rect.top)*dpr;panX=mx-(mx-panX)*(zoom/old);panY=my-(my-panY)*(zoom/old);draw()},{passive:false});
sim.addEventListener('mousedown',e=>{if(e.button===1||e.shiftKey){drag=true;dragStart={x:e.clientX,y:e.clientY,px:panX,py:panY}}});window.addEventListener('mouseup',()=>drag=false);window.addEventListener('mousemove',e=>{if(!drag)return;const dpr=window.devicePixelRatio||1;panX=dragStart.px+(e.clientX-dragStart.x)*dpr;panY=dragStart.py+(e.clientY-dragStart.y)*dpr;draw()});
sim.addEventListener('click',async e=>{if(drag)return;const cell=canvasToCell(e);if(!cell)return;if($('editWalls').checked){const key=cell.r+','+cell.c;if(wallSet.has(key))await command('removewall',{r:cell.r,c:cell.c});else await command('addwall',{r:cell.r,c:cell.c});return}if(!state)return;let found=null;for(const a of state.agents)if(a.r===cell.r&&a.c===cell.c){found=a;break}if(found){selected=found.id;$('agentSearch').value=found.id;updateAgent(found)}});
window.addEventListener('resize',()=>{draw();drawCharts()});
(async()=>{await loadLayout();poll()})().catch(console.error);
</script>
</body>
</html>

)HTML";

#ifdef __linux__
static bool sendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}
#endif

} // namespace

WebUiServer::WebUiServer(int port,
                         LayoutProvider layoutProvider,
                         StateProvider stateProvider,
                         CommandSink commandSink)
    : port_(port),
      layoutProvider_(std::move(layoutProvider)),
      stateProvider_(std::move(stateProvider)),
      commandSink_(std::move(commandSink)) {}

WebUiServer::~WebUiServer() {
    stop();
}

bool WebUiServer::start() {
#ifdef __linux__
    if (serverThread_.joinable()) return true;
    stopping_.store(false);

    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        std::cerr << "UI: socket() failed: " << std::strerror(errno) << "\n";
        return false;
    }

    int yes = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    int flags = ::fcntl(listenFd_, F_GETFL, 0);
    ::fcntl(listenFd_, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "UI: bind(127.0.0.1:" << port_ << ") failed: " << std::strerror(errno) << "\n";
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    if (::listen(listenFd_, 16) < 0) {
        std::cerr << "UI: listen() failed: " << std::strerror(errno) << "\n";
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    serverThread_ = std::thread(&WebUiServer::run, this);
    return true;
#else
    std::cerr << "UI: web dashboard currently supports Linux only.\n";
    return false;
#endif
}

void WebUiServer::stop() {
    stopping_.store(true);
#ifdef __linux__
    if (listenFd_ >= 0) {
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
        listenFd_ = -1;
    }
#endif
    if (serverThread_.joinable()) serverThread_.join();
}

void WebUiServer::run() {
#ifdef __linux__
    while (!stopping_.load()) {
        sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        int client = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                continue;
            }
            if (!stopping_.load()) std::cerr << "UI: accept() failed: " << std::strerror(errno) << "\n";
            break;
        }

        timeval tv{};
        tv.tv_sec = 1;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        std::string req;
        char buf[4096];
        for (;;) {
            ssize_t n = ::recv(client, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, static_cast<size_t>(n));
            if (req.find("\r\n\r\n") != std::string::npos || req.size() > 16384) break;
        }

        std::string response;
        std::istringstream requestStream(req);
        std::string method, target, version;
        requestStream >> method >> target >> version;

        std::string path = target;
        std::string query;
        size_t qpos = target.find('?');
        if (qpos != std::string::npos) {
            path = target.substr(0, qpos);
            query = target.substr(qpos + 1);
        }
        auto params = parseQuery(query);

        try {
            if (method == "GET" && path == "/") {
                response = httpResponse(kDashboardHtml, "text/html; charset=utf-8");
            } else if (method == "GET" && path == "/api/layout") {
                response = httpResponse(layoutJson(layoutProvider_()));
            } else if (method == "GET" && path == "/api/state") {
                int selected = intParam(params, "selected", -1);
                response = httpResponse(stateJson(stateProvider_(selected)));
            } else if (method == "POST" && path == "/api/control") {
                auto it = params.find("cmd");
                if (it == params.end()) {
                    response = httpResponse("{\"ok\":false,\"error\":\"missing cmd\"}",
                                            "application/json; charset=utf-8", "400 Bad Request");
                } else {
                    UiCommand c;
                    bool valid = true;
                    if (it->second == "pause") c.type = UiCommandType::Pause;
                    else if (it->second == "resume") c.type = UiCommandType::Resume;
                    else if (it->second == "stop") c.type = UiCommandType::Stop;
                    else if (it->second == "speed") {
                        c.type = UiCommandType::SetDelayMs;
                        c.value = intParam(params, "ms", 0);
                    } else if (it->second == "freeze") {
                        c.type = UiCommandType::FreezeAgent;
                        c.agentId = intParam(params, "id", -1);
                        c.value = intParam(params, "ms", 1500);
                    } else if (it->second == "unfreeze") {
                        c.type = UiCommandType::UnfreezeAgent;
                        c.agentId = intParam(params, "id", -1);
                    } else if (it->second == "addwall") {
                        c.type = UiCommandType::AddWall;
                        c.r = intParam(params, "r", -1);
                        c.c = intParam(params, "c", -1);
                    } else if (it->second == "removewall") {
                        c.type = UiCommandType::RemoveWall;
                        c.r = intParam(params, "r", -1);
                        c.c = intParam(params, "c", -1);
                    } else valid = false;

                    if (!valid) {
                        response = httpResponse("{\"ok\":false,\"error\":\"unknown command\"}",
                                                "application/json; charset=utf-8", "400 Bad Request");
                    } else {
                        commandSink_(c);
                        response = httpResponse("{\"ok\":true}");
                    }
                }
            } else {
                response = httpResponse("{\"error\":\"not found\"}",
                                        "application/json; charset=utf-8", "404 Not Found");
            }
        } catch (const std::exception& e) {
            response = httpResponse(std::string("{\"ok\":false,\"error\":\"") +
                                    jsonEscape(e.what()) + "\"}",
                                    "application/json; charset=utf-8", "500 Internal Server Error");
        }

        sendAll(client, response);
        ::shutdown(client, SHUT_RDWR);
        ::close(client);
    }
#endif
}
