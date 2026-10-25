#include <Arduino.h>
#include <SD_MMC.h>
#include <WebServer.h>
#include <WiFi.h>
#include <atomic>
#include <cJSON.h>
#include "esp_adc/adc_continuous.h"

constexpr uint32_t kSampleRate = 22050;
constexpr size_t kDmaBufferSize = 4096;
constexpr uint32_t kMaxWavBytes = 0xffffffffU - 44;
WebServer server(80);
adc_continuous_handle_t adcHandle = nullptr;
TaskHandle_t recorderTask = nullptr;
std::atomic<bool> stopRequested{false};
std::atomic<bool> adcOverflow{false};
std::atomic<bool> signalEnabled{false};
std::atomic<bool> signalActive{false};
std::atomic<uint32_t> signalLastPoll{0};
std::atomic<uint32_t> signalSequence{0};
std::atomic<uint16_t> signalRms{0};
std::atomic<uint16_t> signalPeak{0};
std::atomic<uint16_t> signalBias{0};
std::atomic<uint8_t> signalFault{0};
enum class CaptureState { idle, starting, recording };
std::atomic<CaptureState> captureState{CaptureState::idle};
bool sdReady = false;
bool adcReady = false;
bool wifiReady = false;
char recordingName[16] = {};
char lastError[80] = {};
uint8_t dmaBuffer[kDmaBufferSize];
uint8_t pcmBuffer[kDmaBufferSize / 2];

const char kPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="hu"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Radio Recorder</title>
<style>
:root{color-scheme:light;--ink:#24302d;--line:#c8d0ca;--accent:#ad3d34;--muted:#52635a}
*{box-sizing:border-box}body{margin:0;background:repeating-linear-gradient(0deg,#f3f5f2 0 39px,#e9eee9 40px);color:var(--ink);font:16px "Trebuchet MS",sans-serif}
header,main,section,footer{padding:20px max(20px,calc((100vw - 760px)/2))}header{background:#24302d;color:#f6f5ed;font:700 18px Georgia,serif;display:flex;justify-content:space-between;gap:12px;align-items:center}header a{color:inherit;font:16px "Trebuchet MS",sans-serif}
main{padding-top:44px;padding-bottom:40px;border-bottom:1px solid var(--line)}h1{font:normal 36px Georgia,serif;margin:0 0 16px}h2{font:normal 25px Georgia,serif;margin:0 0 20px}
.state{font-size:19px;font-weight:bold;margin:0 0 20px}.note{color:var(--muted);margin:20px 0 0;line-height:1.5}button{font:inherit;cursor:pointer;border:1px solid var(--ink);background:var(--ink);color:white;padding:12px 18px}
button:hover,a:hover{opacity:.8}button:disabled{opacity:.45;cursor:default}#toggle.recording{background:var(--accent);border-color:var(--accent)}
section{padding-top:34px;padding-bottom:34px}ol{padding:0;margin:0;list-style:none}li{border-top:1px solid var(--line);padding:16px 0;display:grid;grid-template-columns:1fr auto;gap:12px;align-items:start}
.filename{display:block;font-weight:bold;overflow-wrap:anywhere}.size{display:block;color:var(--muted);font-size:13px;margin-top:4px}audio{display:block;width:min(100%,400px);height:42px;margin-top:12px}
a{color:#12604f;text-decoration:underline;text-underline-offset:3px}#more{margin-top:20px}footer{color:var(--muted);border-top:1px solid var(--line);font-size:13px}
.signalbar{display:flex;align-items:center;justify-content:space-between;gap:16px;flex-wrap:wrap}.signalbar h2{margin:0}.signalvalues{color:var(--muted);margin:14px 0;font-variant-numeric:tabular-nums}
#signalChart{width:100%;height:200px;display:block;background:#f8faf7;border:1px solid var(--line)}.legend{display:flex;gap:20px;color:var(--muted);font-size:13px;margin-top:10px}.legend b{font-weight:normal}
.legend b:first-child{color:#12604f}.legend b:last-child{color:var(--accent)}
@media(max-width:560px){li{grid-template-columns:1fr}li a{justify-self:start}h1{font-size:30px}}
</style></head><body><header>RADIO RECORDER <a href="/files">Fájlkezelő</a></header>
<main><h1>Felvétel</h1><p class="state" id="state">Kapcsolódás...</p><button id="toggle" disabled>Felvétel indítása</button><p class="note" id="message"></p>
<p class="note">Csak ellenőrzött, kis szintű tesztforrás. A rádiót még ne csatlakoztasd.</p></main>
<section><div class="signalbar"><h2>GPIO1 jel</h2><button id="signalToggle" disabled>Mérés bekapcsolása</button></div>
<p id="signalValues" class="signalvalues">Kikapcsolva</p><div id="signalPlot" hidden><canvas id="signalChart" aria-label="GPIO1 RMS és csúcs idősor" role="img"></canvas>
<div class="legend"><b>RMS</b><b>Csúcs</b><span>ADC egység / 30 s</span></div></div></section>
<section><h2>Mentett felvételek</h2><p id="empty">Betöltés...</p><ol id="files"></ol><button id="more" hidden>Továbbiak</button></section>
<footer>ESP32-S3 / microSD</footer>
<script>
const stateEl=document.querySelector('#state'),toggle=document.querySelector('#toggle'),message=document.querySelector('#message');
const files=document.querySelector('#files'),empty=document.querySelector('#empty'),more=document.querySelector('#more');
const signalToggle=document.querySelector('#signalToggle'),signalValues=document.querySelector('#signalValues');
const signalPlot=document.querySelector('#signalPlot'),signalChart=document.querySelector('#signalChart');
let currentState='',nextOffset=null,loaded=false,listing=false;
let meterRunning=false,meterGeneration=0,meterTimer=null,lastSignalSequence=-1;
const signalHistory=[];
function drawSignal(){
 const width=signalChart.clientWidth,height=200,ratio=devicePixelRatio||1,context=signalChart.getContext('2d');
 signalChart.width=Math.round(width*ratio);signalChart.height=Math.round(height*ratio);context.setTransform(ratio,0,0,ratio,0,0);
 context.clearRect(0,0,width,height);const left=42,right=width-12,top=15,bottom=height-28;
 const maximum=Math.min(4096,Math.max(128,2**Math.ceil(Math.log2(Math.max(1,...signalHistory.map(point=>point.peak))*1.2))));
 context.font='12px "Trebuchet MS",sans-serif';context.lineWidth=1;context.textAlign='right';
 for(let step=0;step<=4;step++){
  const ordinate=bottom-(bottom-top)*step/4;
  context.strokeStyle='#d8e0d9';context.beginPath();context.moveTo(left,ordinate);context.lineTo(right,ordinate);context.stroke();
  context.fillStyle='#52635a';context.fillText(String(Math.round(maximum*step/4)),left-7,ordinate+4);
 }
 for(const [key,color] of [['rms','#12604f'],['peak','#ad3d34']]){
  context.strokeStyle=color;context.lineWidth=2;context.beginPath();
  signalHistory.forEach((point,index)=>{const x=right-(signalHistory.length-1-index)*(right-left)/119;
   const y=bottom-Math.min(maximum,point[key])*(bottom-top)/maximum;
   if(index)context.lineTo(x,y);else context.moveTo(x,y);
  });context.stroke();
 }
}
async function pollSignal(generation){
 if(!meterRunning||generation!==meterGeneration)return;
 try{
  const response=await fetch('/api/signal',{cache:'no-store'});if(!response.ok)throw Error();
  const data=await response.json();if(generation!==meterGeneration)return;
  if(!data.enabled){meterRunning=false;signalPlot.hidden=true;signalToggle.textContent='Mérés bekapcsolása';signalValues.textContent=data.fault?'ADC mérési hiba ('+data.fault+')':'Kikapcsolva';return}
  signalToggle.textContent='Mérés kikapcsolása';signalPlot.hidden=false;
  if(data.active&&data.sequence!==lastSignalSequence&&data.sequence>0){
   lastSignalSequence=data.sequence;signalHistory.push({rms:data.rms,peak:data.peak});if(signalHistory.length>120)signalHistory.shift();drawSignal();
  }
  signalValues.textContent=data.active&&data.sequence>0?'RMS: '+data.rms+' | Csúcs: '+data.peak+' | DC-közép: '+data.bias+' (ADC)':'Mérés indul...';
 }catch{signalValues.textContent='A mérési adatok nem elérhetők.'}
 finally{if(meterRunning&&generation===meterGeneration)meterTimer=setTimeout(()=>pollSignal(generation),250)}
}
signalToggle.addEventListener('click',async()=>{
 signalToggle.disabled=true;const turnOn=!meterRunning,previous=meterRunning;
 ++meterGeneration;clearTimeout(meterTimer);
 try{
  const response=await fetch(turnOn?'/api/signal/on':'/api/signal/off',{method:'POST'});if(!response.ok)throw Error();
  meterRunning=turnOn;
  if(turnOn){signalHistory.length=0;lastSignalSequence=-1;signalPlot.hidden=false;drawSignal();pollSignal(meterGeneration)}
  else{signalPlot.hidden=true;signalValues.textContent='Kikapcsolva';signalToggle.textContent='Mérés bekapcsolása'}
 }catch{meterRunning=previous;signalValues.textContent='A mérés kapcsolása sikertelen.'}
 signalToggle.disabled=false;
});
addEventListener('pagehide',()=>{if(meterRunning)navigator.sendBeacon('/api/signal/off')});
addEventListener('resize',()=>{if(meterRunning)drawSignal()});
async function list(reset){
 if(listing)return;
 listing=true;
 try{
 if(reset){files.replaceChildren();nextOffset=0;loaded=false}
 if(nextOffset===null)return;
 const response=await fetch('/api/recordings?offset='+nextOffset,{cache:'no-store'});
 if(!response.ok)return;
 const data=await response.json();
 for(const item of data.files){
  const row=document.createElement('li'),info=document.createElement('div'),title=document.createElement('span'),size=document.createElement('span');
  title.className='filename';title.textContent=item.name;size.className='size';size.textContent=(item.bytes/1024).toFixed(0)+' kB';
  const audio=document.createElement('audio');audio.controls=true;audio.preload='none';audio.src='/file?name='+encodeURIComponent(item.name);
  info.append(title,size,audio);
  const download=document.createElement('a');download.href=audio.src+'&download=1';download.download=item.name;download.textContent='Letöltés';
  row.append(info,download);files.append(row);
 }
 nextOffset=data.next;more.hidden=nextOffset===null;empty.hidden=files.children.length>0;empty.textContent='Nincs mentett felvétel.';loaded=true;
 }finally{listing=false}
}
async function refresh(){
 try{
  const response=await fetch('/api/status',{cache:'no-store'});if(!response.ok)throw Error();
  const data=await response.json();
  const busy=data.state!=='idle';
  stateEl.textContent=!data.ready?'SD vagy ADC nem elérhető':busy?(data.state==='starting'?'Felvétel indul...':'Felvétel folyamatban'):(data.hasRecordings?'Van mentett felvétel':'Nincs mentett felvétel');
  toggle.textContent=busy?'Felvétel leállítása':'Felvétel indítása';toggle.classList.toggle('recording',busy);
  toggle.disabled=!data.ready||data.state==='starting'||(busy&&data.stopping);
  signalToggle.disabled=!data.meterReady&&!meterRunning;
  message.textContent=data.error||'';
  if(!busy&&(!loaded||currentState!=='idle'))await list(true);
  currentState=data.state;
 }catch{stateEl.textContent='A kapcsolat megszakadt';toggle.disabled=true}
}
toggle.addEventListener('click',async()=>{
 toggle.disabled=true;
 try{const response=await fetch(currentState==='idle'?'/api/start':'/api/stop',{method:'POST'});if(!response.ok)message.textContent=await response.text();}
 catch{message.textContent='A kérés nem sikerült.'}
 await refresh();
});
more.addEventListener('click',()=>list(false));
refresh();setInterval(refresh,1500);
fetch('/api/signal',{cache:'no-store'}).then(response=>response.json()).then(data=>{
 if(data.enabled){meterRunning=true;signalToggle.textContent='Mérés kikapcsolása';signalPlot.hidden=false;pollSignal(meterGeneration)}
}).catch(()=>{});
</script></body></html>)HTML";

const char kFilesPage[] PROGMEM = R"FILES(<!doctype html>
<html lang="hu"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Fájlkezelő - Radio Recorder</title>
<style>
:root{color-scheme:light;--ink:#24302d;--line:#c8d0ca;--muted:#52635a;--paper:#f3f5f2}
*{box-sizing:border-box}body{margin:0;background:repeating-linear-gradient(0deg,#f3f5f2 0 39px,#e9eee9 40px);color:var(--ink);font:16px "Trebuchet MS",sans-serif}
header,main,footer{padding:20px max(20px,calc((100vw - 760px)/2))}header{background:var(--ink);color:#f6f5ed;display:flex;align-items:center;justify-content:space-between;gap:12px;flex-wrap:wrap;font:700 18px Georgia,serif}
header a{color:inherit;font:16px "Trebuchet MS",sans-serif}main{padding-top:36px;min-height:70vh}h1{font:normal 34px Georgia,serif;margin:0 0 12px}
a{color:#12604f;text-underline-offset:3px}a:hover,button:hover{opacity:.8}.crumbs{display:flex;flex-wrap:wrap;gap:8px;margin:0 0 28px;padding:0;list-style:none;overflow-wrap:anywhere}
.crumbs li:not(:last-child):after{content:"/";color:var(--muted);margin-left:8px}.toolbar{display:flex;align-items:center;justify-content:space-between;border-bottom:2px solid var(--ink);padding-bottom:12px;gap:12px}
button{cursor:pointer;background:var(--ink);color:white;border:1px solid var(--ink);font:inherit;padding:9px 14px}button:disabled{opacity:.5;cursor:default}
.items{padding:0;margin:0;list-style:none}.items li{padding:15px 0;border-bottom:1px solid var(--line);display:flex;justify-content:space-between;align-items:flex-start;gap:16px}
.name{font-weight:700;overflow-wrap:anywhere}.meta{color:var(--muted);font-size:13px;margin:4px 0 0}.actions{display:flex;align-items:center;gap:16px;flex-shrink:0}audio{width:min(100%,400px);height:40px;display:block;margin-top:12px}
#message{color:var(--muted);margin:20px 0;line-height:1.4}#more{margin-top:22px}footer{border-top:1px solid var(--line);color:var(--muted);font-size:13px}
@media(max-width:520px){.items li{display:block}.actions{margin-top:12px}.toolbar{align-items:flex-start;flex-direction:column}h1{font-size:29px}}
</style></head><body><header>RADIO RECORDER <a href="/">Felvétel</a></header>
<main><h1>Fájlkezelő</h1><ol id="crumbs" class="crumbs"></ol>
<div class="toolbar"><strong id="location">microSD</strong><button id="reload" title="Lista frissítése">Frissítés</button></div>
<p id="message">Betöltés...</p><ol id="items" class="items"></ol><button id="more" hidden>Továbbiak</button></main>
<footer>Lejátszás a böngésző által támogatott hangformátumokkal.</footer>
<script>
const folder=new URLSearchParams(location.search).get('dir')||'/';
const listEl=document.querySelector('#items'),message=document.querySelector('#message'),more=document.querySelector('#more');
let next=0,loading=false;
const sound=name=>/\.(wav|mp3|m4a|mp4|ogg|oga|opus|flac|aac|webm)$/i.test(name);
const link=path=>'/files?dir='+encodeURIComponent(path);
function breadcrumbs(){
 const crumbs=document.querySelector('#crumbs');let path='';
 const root=document.createElement('li'),rootLink=document.createElement('a');rootLink.href=link('/');rootLink.textContent='microSD';root.append(rootLink);crumbs.append(root);
 for(const name of folder.split('/').filter(Boolean)){
  path+='/'+name;const part=document.createElement('li'),anchor=document.createElement('a');
  anchor.textContent=name;anchor.href=link(path);part.append(anchor);crumbs.append(part);
 }
 document.querySelector('#location').textContent=folder;
}
function render(item){
 const row=document.createElement('li'),info=document.createElement('div'),name=document.createElement('div');name.className='name';
 if(item.directory){const anchor=document.createElement('a');anchor.href=link(item.path);anchor.textContent=item.name;name.append(anchor)}
 else name.textContent=item.name;
 const meta=document.createElement('div');meta.className='meta';meta.textContent=item.directory?'Mappa':(item.bytes/1024).toFixed(0)+' kB';
 info.append(name,meta);row.append(info);
 if(!item.directory){
  const actions=document.createElement('div');actions.className='actions';
  if(sound(item.name)){
   const play=document.createElement('button');play.textContent='Lejátszás';play.addEventListener('click',()=>{
    if(info.querySelector('audio')){info.querySelector('audio').remove();play.textContent='Lejátszás';return}
    const audio=document.createElement('audio');audio.controls=true;audio.src='/file?path='+encodeURIComponent(item.path);info.append(audio);
    play.textContent='Bezárás';audio.play().catch(()=>{});
   });actions.append(play);
  }
  const download=document.createElement('a');download.href='/file?path='+encodeURIComponent(item.path)+'&download=1';
  download.download=item.name;download.textContent='Letöltés';actions.append(download);row.append(actions);
 }
 listEl.append(row);
}
async function load(reset=false){
 if(loading)return;loading=true;
 if(reset){listEl.replaceChildren();next=0}
 try{
  const response=await fetch('/api/files?dir='+encodeURIComponent(folder)+'&offset='+next,{cache:'no-store'});
  if(!response.ok)throw Error(response.status===409?'Felvétel alatt a fájlok nem olvashatók.':'A mappa nem elérhető.');
  const data=await response.json();data.items.forEach(render);next=data.next;
  more.hidden=next===null;message.textContent=listEl.children.length?'':'Üres mappa.';
 }catch(error){message.textContent=error.message;more.hidden=true}
 finally{loading=false}
}
document.querySelector('#reload').addEventListener('click',()=>load(true));
more.addEventListener('click',()=>load());breadcrumbs();load();
</script></body></html>)FILES";

void put16(uint8_t *destination, uint16_t value) {
  destination[0] = value & 0xff;
  destination[1] = value >> 8;
}

void put32(uint8_t *destination, uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) destination[shift / 8] = value >> shift;
}

bool writeWavHeader(File &file, uint32_t dataBytes) {
  uint8_t header[44] = {
      'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E',
      'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0,
      'd', 'a', 't', 'a', 0, 0, 0, 0};
  put32(header + 4, dataBytes + 36);
  put32(header + 24, kSampleRate);
  put32(header + 28, kSampleRate * 2);
  put32(header + 40, dataBytes);
  return file.seek(0) && file.write(header, sizeof(header)) == sizeof(header);
}

bool adcPoolOverflow(adc_continuous_handle_t, const adc_continuous_evt_data_t *, void *) {
  adcOverflow.store(true, std::memory_order_relaxed);
  return false;
}

struct SignalWindow {
  uint64_t squares = 0;
  uint32_t rawSum = 0;
  uint32_t samples = 0;
  uint16_t peak = 0;
};

bool signalRequested() {
  if (!signalEnabled.load()) return false;
  if (static_cast<uint32_t>(millis() - signalLastPoll.load()) <= 4000) return true;
  signalEnabled.store(false);
  return false;
}

void addSignalSample(SignalWindow &window, uint16_t raw, int32_t centered) {
  uint16_t amplitude = abs(centered);
  window.squares += static_cast<uint64_t>(amplitude) * amplitude;
  window.rawSum += raw;
  if (amplitude > window.peak) window.peak = amplitude;
  if (++window.samples >= kSampleRate / 10) {
    signalRms.store(static_cast<uint16_t>(sqrtf(static_cast<float>(window.squares) / window.samples)));
    signalPeak.store(window.peak);
    signalBias.store(window.rawSum / window.samples);
    signalSequence.fetch_add(1);
    window = {};
  }
}

bool nextRecordingName() {
  for (uint32_t number = 1; number <= 99999; ++number) {
    snprintf(recordingName, sizeof(recordingName), "REC%05lu.wav", static_cast<unsigned long>(number));
    if (!SD_MMC.exists(String("/") + recordingName)) return true;
  }
  return false;
}

bool startRecording() {
  if (!sdReady || !adcReady || recorderTask == nullptr || captureState.load() != CaptureState::idle) return false;
  if (!nextRecordingName()) {
    snprintf(lastError, sizeof(lastError), "No free recording filename");
    return false;
  }
  lastError[0] = '\0';
  stopRequested.store(false);
  captureState.store(CaptureState::starting);
  xTaskNotifyGive(recorderTask);
  return true;
}

bool stopRecording() {
  if (captureState.load() == CaptureState::idle) return false;
  stopRequested.store(true);
  return true;
}

bool isRecordingFile(const String &name) {
  if (name.length() != 12) return false;
  String upper = name;
  upper.toUpperCase();
  if (!upper.startsWith("REC") || !upper.endsWith(".WAV")) return false;
  for (int index = 3; index < 8; ++index) if (!isDigit(name[index])) return false;
  return true;
}

String fileName(const File &file) {
  String name = file.name();
  return name.substring(name.lastIndexOf('/') + 1);
}

bool hasRecordings() {
  File directory = SD_MMC.open("/");
  if (!directory) return false;
  File entry;
  while ((entry = directory.openNextFile())) {
    bool found = !entry.isDirectory() && isRecordingFile(fileName(entry)) && entry.size() >= 44;
    entry.close();
    if (found) return true;
  }
  return false;
}

void sendStatus() {
  CaptureState state = captureState.load();
  bool ready = sdReady && adcReady && recorderTask != nullptr;
  String json = "{\"ready\":" + String(ready ? "true" : "false");
  json += ",\"meterReady\":" + String(adcReady && recorderTask ? "true" : "false");
  json += ",\"state\":\"" + String(state == CaptureState::idle ? "idle" : state == CaptureState::starting ? "starting" : "recording") + "\"";
  json += ",\"stopping\":" + String(stopRequested.load() ? "true" : "false");
  json += ",\"hasRecordings\":" + String(ready && state == CaptureState::idle && hasRecordings() ? "true" : "false");
  json += ",\"error\":\"" + String(state == CaptureState::idle ? lastError : "") + "\"}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void sendRecordings() {
  if (!sdReady || captureState.load() != CaptureState::idle) {
    server.send(409, "text/plain", "SD unavailable or recording in progress");
    return;
  }
  uint32_t offset = max(0L, server.arg("offset").toInt());
  File directory = SD_MMC.open("/");
  if (!directory) {
    server.send(500, "text/plain", "Cannot read SD card");
    return;
  }
  String json = "{\"files\":[";
  uint32_t index = 0;
  uint32_t shown = 0;
  bool moreFiles = false;
  File entry;
  while ((entry = directory.openNextFile())) {
    String name = fileName(entry);
    if (!entry.isDirectory() && isRecordingFile(name) && entry.size() >= 44) {
      if (index >= offset && shown < 20) {
        if (shown) json += ',';
        json += "{\"name\":\"" + name + "\",\"bytes\":" + String(entry.size()) + "}";
        ++shown;
      } else if (index >= offset + 20) {
        moreFiles = true;
        entry.close();
        break;
      }
      ++index;
    }
    entry.close();
  }
  directory.close();
  json += moreFiles ? "],\"next\":" + String(offset + shown) + "}" : "],\"next\":null}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

bool safeSdPath(const String &path) {
  if (path.isEmpty() || path.length() > 220 || path[0] != '/') return false;
  if (path == "/") return true;
  int segmentStart = 1;
  for (size_t index = 1; index <= path.length(); ++index) {
    if (index < path.length() && static_cast<uint8_t>(path[index]) < 32) return false;
    if (index == path.length() || path[index] == '/') {
      String segment = path.substring(segmentStart, index);
      if (segment.isEmpty() || segment == "." || segment == "..") return false;
      segmentStart = index + 1;
    } else if (path[index] == '\\' || path[index] == ':' || path[index] == '"' || path[index] == 127) {
      return false;
    }
  }
  return true;
}

void sendFiles() {
  if (!sdReady || captureState.load() != CaptureState::idle) {
    server.send(409, "text/plain", "SD unavailable or recording in progress");
    return;
  }
  String directoryPath = server.hasArg("dir") ? server.arg("dir") : "/";
  if (!safeSdPath(directoryPath)) {
    server.send(400, "text/plain", "Invalid directory");
    return;
  }
  File directory = SD_MMC.open(directoryPath, FILE_READ);
  if (!directory || !directory.isDirectory()) {
    server.send(404, "text/plain", "Directory not found");
    return;
  }
  uint32_t offset = max(0L, server.arg("offset").toInt());
  cJSON *result = cJSON_CreateObject();
  cJSON *items = result ? cJSON_AddArrayToObject(result, "items") : nullptr;
  if (!items) {
    cJSON_Delete(result);
    server.send(500, "text/plain", "Cannot list SD card");
    return;
  }
  uint32_t index = 0;
  uint32_t shown = 0;
  bool moreFiles = false;
  File entry;
  while ((entry = directory.openNextFile())) {
    if (index >= offset && shown < 20) {
      String name = fileName(entry);
      String path = directoryPath == "/" ? "/" + name : directoryPath + "/" + name;
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "name", name.c_str());
      cJSON_AddStringToObject(item, "path", path.c_str());
      cJSON_AddBoolToObject(item, "directory", entry.isDirectory());
      cJSON_AddNumberToObject(item, "bytes", entry.isDirectory() ? 0 : entry.size());
      cJSON_AddItemToArray(items, item);
      ++shown;
    } else if (index >= offset + 20) {
      moreFiles = true;
      entry.close();
      break;
    }
    ++index;
    entry.close();
  }
  directory.close();
  if (moreFiles) cJSON_AddNumberToObject(result, "next", offset + shown);
  else cJSON_AddNullToObject(result, "next");
  char *json = cJSON_PrintUnformatted(result);
  if (json) {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
    cJSON_free(json);
  } else {
    server.send(500, "text/plain", "Cannot list SD card");
  }
  cJSON_Delete(result);
}

void sendSignal() {
  if (signalEnabled.load()) signalLastPoll.store(millis());
  String json = "{\"enabled\":" + String(signalEnabled.load() ? "true" : "false");
  json += ",\"active\":" + String(signalActive.load() ? "true" : "false");
  json += ",\"sequence\":" + String(signalSequence.load());
  json += ",\"rms\":" + String(signalRms.load());
  json += ",\"peak\":" + String(signalPeak.load());
  json += ",\"bias\":" + String(signalBias.load());
  json += ",\"fault\":" + String(signalFault.load()) + "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

void setSignal(bool enabled) {
  if (!adcReady || !recorderTask) {
    server.send(503, "text/plain", "ADC unavailable");
    return;
  }
  if (enabled) {
    signalSequence.store(0);
    signalFault.store(0);
    signalLastPoll.store(millis());
  }
  signalEnabled.store(enabled);
  xTaskNotifyGive(recorderTask);
  server.send(200, "text/plain", enabled ? "Monitoring" : "Stopped");
}

bool parseByteOffset(const String &text, uint32_t &value) {
  if (text.isEmpty()) return false;
  uint64_t parsed = 0;
  for (size_t index = 0; index < text.length(); ++index) {
    if (!isDigit(text[index])) return false;
    parsed = parsed * 10 + (text[index] - '0');
    if (parsed > 0xffffffffU) return false;
  }
  value = static_cast<uint32_t>(parsed);
  return true;
}

const char *audioMime(const String &name) {
  String lower = name;
  lower.toLowerCase();
  if (lower.endsWith(".wav")) return "audio/wav";
  if (lower.endsWith(".mp3")) return "audio/mpeg";
  if (lower.endsWith(".m4a") || lower.endsWith(".mp4")) return "audio/mp4";
  if (lower.endsWith(".ogg") || lower.endsWith(".oga") || lower.endsWith(".opus")) return "audio/ogg";
  if (lower.endsWith(".flac")) return "audio/flac";
  if (lower.endsWith(".aac")) return "audio/aac";
  if (lower.endsWith(".webm")) return "audio/webm";
  return nullptr;
}

void sendFile() {
  String path = server.hasArg("path") ? server.arg("path") : String("/") + server.arg("name");
  if (!sdReady || captureState.load() != CaptureState::idle) {
    server.send(409, "text/plain", "Recording in progress or SD unavailable");
    return;
  }
  if (!safeSdPath(path) || path == "/") {
    server.send(400, "text/plain", "Invalid file path");
    return;
  }
  File file = SD_MMC.open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    server.send(404, "text/plain", "File not found");
    return;
  }
  String name = fileName(file);
  const char *mime = audioMime(name);
  if (!mime) mime = "application/octet-stream";
  uint32_t total = file.size();
  server.sendHeader("Accept-Ranges", "bytes");
  if (server.arg("download") == "1" || strcmp(mime, "application/octet-stream") == 0) {
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  }
  server.sendHeader("Cache-Control", "no-store");
  String range = server.header("Range");
  if (range.isEmpty()) {
    server.streamFile(file, mime);
    file.close();
    return;
  }
  int separator = range.indexOf('-');
  uint32_t start = 0;
  uint32_t end = total - 1;
  uint32_t amount = 0;
  bool valid = range.startsWith("bytes=") && separator >= 6 && range.indexOf(',', 6) == -1;
  if (valid && separator == 6) {
    valid = parseByteOffset(range.substring(separator + 1), amount) && amount > 0;
    if (valid) start = amount >= total ? 0 : total - amount;
  } else if (valid) {
    valid = parseByteOffset(range.substring(6, separator), start);
    if (valid && separator + 1 < range.length()) valid = parseByteOffset(range.substring(separator + 1), end);
  }
  if (!valid || start >= total || end < start) {
    server.sendHeader("Content-Range", "bytes */" + String(total));
    server.send(416, "text/plain", "Invalid byte range");
    return;
  }
  if (end >= total) end = total - 1;
  if (end - start >= 128 * 1024 && server.arg("download") != "1") end = start + 128 * 1024 - 1;
  server.sendHeader("Content-Range", "bytes " + String(start) + "-" + String(end) + "/" + String(total));
  server.setContentLength(end - start + 1);
  server.send(206, mime, "");
  if (!file.seek(start)) return;
  auto &client = server.client();
  uint32_t remaining = end - start + 1;
  uint8_t buffer[1024];
  while (remaining && client.connected()) {
    size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    size_t read = file.read(buffer, chunk);
    if (!read) break;
    size_t sent = 0;
    while (sent < read && client.connected()) {
      size_t count = client.write(buffer + sent, read - sent);
      if (!count) break;
      sent += count;
    }
    if (sent != read) break;
    remaining -= sent;
  }
  file.close();
}

void initWeb() {
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP("RadioRecorder")) {
    Serial.println("Cannot start Wi-Fi AP");
    return;
  }
  server.on("/", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", kPage); });
  server.on("/files", HTTP_GET, []() { server.send_P(200, "text/html; charset=utf-8", kFilesPage); });
  server.on("/api/status", HTTP_GET, sendStatus);
  server.on("/api/recordings", HTTP_GET, sendRecordings);
  server.on("/api/files", HTTP_GET, sendFiles);
  server.on("/api/signal", HTTP_GET, sendSignal);
  server.on("/api/signal/on", HTTP_POST, []() { setSignal(true); });
  server.on("/api/signal/off", HTTP_POST, []() { setSignal(false); });
  server.on("/file", HTTP_GET, sendFile);
  server.on("/api/start", HTTP_POST, []() {
    if (startRecording()) server.send(200, "text/plain", recordingName);
    else server.send(409, "text/plain", lastError[0] ? lastError : "SD, ADC or recorder unavailable");
  });
  server.on("/api/stop", HTTP_POST, []() {
    if (stopRecording()) server.send(200, "text/plain", "Stopping");
    else server.send(409, "text/plain", "Not recording");
  });
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  const char *headers[] = {"Range"};
  server.collectHeaders(headers, 1);
  server.begin();
  wifiReady = true;
}

void printSetupStatus() {
  Serial.printf("SD card: %s\n", sdReady ? "ready" : "unavailable");
  Serial.printf("ADC GPIO1: %s\n", adcReady ? "ready" : "unavailable");
  Serial.printf("Recorder: %s\n", sdReady && recorderTask ? "ready" : "unavailable");
  if (wifiReady) {
    Serial.print("Wi-Fi AP SSID: ");
    Serial.println(WiFi.softAPSSID());
    Serial.print("Wi-Fi AP IP: http://");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("Wi-Fi AP: unavailable");
  }
  if (captureState.load() != CaptureState::idle) Serial.printf("Recording: %s\n", recordingName);
}

void monitorIdle() {
  adcOverflow.store(false, std::memory_order_relaxed);
  if (adc_continuous_flush_pool(adcHandle) != ESP_OK || adc_continuous_start(adcHandle) != ESP_OK) {
    signalFault.store(1);
    signalEnabled.store(false);
    return;
  }
  signalActive.store(true);
  SignalWindow window;
  float dcLevel = 2048.0f;
  while (signalRequested() && captureState.load() == CaptureState::idle) {
    if (adcOverflow.load(std::memory_order_relaxed)) {
      signalFault.store(2);
      signalEnabled.store(false);
      break;
    }
    uint32_t length = 0;
    esp_err_t result = adc_continuous_read(adcHandle, dmaBuffer, sizeof(dmaBuffer), &length, 100);
    if (result == ESP_ERR_TIMEOUT) continue;
    if (result != ESP_OK || length % SOC_ADC_DIGI_RESULT_BYTES != 0) {
      signalFault.store(3);
      signalEnabled.store(false);
      break;
    }
    uint32_t validSamples = 0;
    for (uint32_t offset = 0; offset < length; offset += SOC_ADC_DIGI_RESULT_BYTES) {
      adc_digi_output_data_t sample;
      memcpy(&sample, dmaBuffer + offset, sizeof(sample));
      if (sample.type2.unit != ADC_UNIT_1 || sample.type2.channel != ADC_CHANNEL_0) continue;
      ++validSamples;
      dcLevel += (static_cast<float>(sample.type2.data) - dcLevel) / 1024.0f;
      addSignalSample(window, sample.type2.data, static_cast<int32_t>(sample.type2.data - dcLevel));
    }
    if (!validSamples) {
      signalFault.store(3);
      signalEnabled.store(false);
      break;
    }
  }
  adc_continuous_stop(adcHandle);
  signalActive.store(false);
}

void recordLoop(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (captureState.load() == CaptureState::idle && signalRequested()) monitorIdle();
    if (captureState.load() != CaptureState::starting) continue;
    adcOverflow.store(false, std::memory_order_relaxed);
    String path = String("/") + recordingName;
    File file = SD_MMC.open(path, FILE_WRITE);
    uint32_t dataBytes = 0;
    bool started = false;
    bool valid = false;
    if (!file || !writeWavHeader(file, 0)) {
      snprintf(lastError, sizeof(lastError), "Cannot create WAV file");
    } else if (adc_continuous_flush_pool(adcHandle) != ESP_OK || adc_continuous_start(adcHandle) != ESP_OK) {
      snprintf(lastError, sizeof(lastError), "Cannot start ADC");
    } else {
      started = true;
      signalActive.store(true);
      captureState.store(CaptureState::recording);
      Serial.printf("Recording started: %s\n", recordingName);
      float dcLevel = 2048.0f;
      SignalWindow window;
      while (!stopRequested.load()) {
        bool publishSignal = signalRequested();
        if (adcOverflow.load(std::memory_order_relaxed)) {
          snprintf(lastError, sizeof(lastError), "ADC buffer overflow: samples lost");
          break;
        }
        uint32_t length = 0;
        esp_err_t result = adc_continuous_read(adcHandle, dmaBuffer, sizeof(dmaBuffer), &length, 100);
        if (result == ESP_ERR_TIMEOUT) continue;
        if (result != ESP_OK || length % SOC_ADC_DIGI_RESULT_BYTES != 0) {
          snprintf(lastError, sizeof(lastError), "ADC read failed or samples lost");
          break;
        }
        size_t pcmLength = 0;
        for (uint32_t offset = 0; offset < length; offset += SOC_ADC_DIGI_RESULT_BYTES) {
          adc_digi_output_data_t sample;
          memcpy(&sample, dmaBuffer + offset, sizeof(sample));
          if (sample.type2.unit != ADC_UNIT_1 || sample.type2.channel != ADC_CHANNEL_0) continue;
          dcLevel += (static_cast<float>(sample.type2.data) - dcLevel) / 1024.0f;
          float centered = static_cast<float>(sample.type2.data) - dcLevel;
          if (publishSignal) addSignalSample(window, sample.type2.data, static_cast<int32_t>(centered));
          int32_t value = static_cast<int32_t>(centered * 16);
          value = constrain(value, -32768, 32767);
          put16(pcmBuffer + pcmLength, static_cast<uint16_t>(static_cast<int16_t>(value)));
          pcmLength += 2;
        }
        if (pcmLength == 0) {
          snprintf(lastError, sizeof(lastError), "ADC returned no valid samples");
          break;
        }
        if (dataBytes > kMaxWavBytes - pcmLength || file.write(pcmBuffer, pcmLength) != pcmLength) {
          snprintf(lastError, sizeof(lastError), "SD write failed or WAV size limit reached");
          break;
        }
        dataBytes += pcmLength;
      }
    }
    if (started) {
      adc_continuous_stop(adcHandle);
      signalActive.store(false);
    }
    if (adcOverflow.load(std::memory_order_relaxed) && !lastError[0]) {
      snprintf(lastError, sizeof(lastError), "ADC buffer overflow: samples lost");
    }
    if (file) {
      valid = writeWavHeader(file, dataBytes);
      file.close();
    }
    if (!valid) {
      SD_MMC.remove(path);
      if (!lastError[0]) snprintf(lastError, sizeof(lastError), "Cannot finish WAV file");
    }
    if (valid) Serial.printf("Recording stopped: %s (%lu audio bytes)\n", recordingName, static_cast<unsigned long>(dataBytes));
    else Serial.printf("Recording failed: %s\n", recordingName);
    if (lastError[0]) Serial.printf("Recording error: %s\n", lastError);
    captureState.store(CaptureState::idle);
    if (signalRequested()) xTaskNotifyGive(recorderTask);
  }
}

bool initAdc() {
  adc_continuous_handle_cfg_t handleConfig = {};
  handleConfig.max_store_buf_size = 32768;
  handleConfig.conv_frame_size = 1024;
  if (adc_continuous_new_handle(&handleConfig, &adcHandle) != ESP_OK) return false;

  adc_digi_pattern_config_t pattern = {};
  pattern.atten = ADC_ATTEN_DB_12;
  pattern.channel = ADC_CHANNEL_0;
  pattern.unit = ADC_UNIT_1;
  pattern.bit_width = ADC_BITWIDTH_12;

  adc_continuous_config_t config = {};
  config.pattern_num = 1;
  config.adc_pattern = &pattern;
  config.sample_freq_hz = kSampleRate;
  config.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  config.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
  adc_continuous_evt_cbs_t callbacks = {};
  callbacks.on_pool_ovf = adcPoolOverflow;
  if (adc_continuous_config(adcHandle, &config) == ESP_OK &&
      adc_continuous_register_event_callbacks(adcHandle, &callbacks, nullptr) == ESP_OK) return true;
  adc_continuous_deinit(adcHandle);
  adcHandle = nullptr;
  return false;
}

void setup() {
  Serial.begin(115200);
  Serial.println("RadioRecorder setup starting");
  sdReady = SD_MMC.setPins(39, 38, 40) && SD_MMC.begin("/sdcard", true, false) && SD_MMC.cardType() != CARD_NONE;
  adcReady = initAdc();
  if (adcReady && xTaskCreatePinnedToCore(recordLoop, "recorder", 8192, nullptr, 2, &recorderTask, 1) != pdPASS) {
    Serial.println("Cannot create recorder task");
  }
  initWeb();
  printSetupStatus();
}

void loop() {
  server.handleClient();
  if (Serial.available()) {
    char command = Serial.read();
    if (command == 'r' && !startRecording()) Serial.println("Cannot start recording");
    if (command == 's') Serial.println(stopRecording() ? "Stop requested" : "Not recording");
    if (command == 'i') printSetupStatus();
  }
  delay(10);
}