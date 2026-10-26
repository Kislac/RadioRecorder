#include <Arduino.h>
#include <SD_MMC.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <atomic>
#include <cJSON.h>
#include "esp_adc/adc_continuous.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"

constexpr uint32_t kSampleRate = 22050;
constexpr int kAudioGpio = 1;
constexpr gpio_num_t kSpeakerBclkGpio = GPIO_NUM_42;
constexpr gpio_num_t kSpeakerLrcGpio = GPIO_NUM_41;
constexpr gpio_num_t kSpeakerDataGpio = GPIO_NUM_48;
constexpr gpio_num_t kPdmOutGpio = GPIO_NUM_2;
constexpr int kEncoderClkGpio = 21;
constexpr int kEncoderDtGpio = 47;
constexpr int kEncoderSwGpio = 14;
constexpr char kFirmwareVersion[] = "0.1.12";
constexpr uint32_t kFirmwareBuild = FIRMWARE_BUILD;
constexpr size_t kDmaBufferSize = 4096;
constexpr size_t kLiveFrameBytes = 1024;
constexpr uint32_t kMaxWavBytes = 0xffffffffU - 44;
WebServer server(80);
WebSocketsServer audioSocket(81);
struct AudioFrame {
  uint16_t length;
  uint8_t samples[kLiveFrameBytes];
};
QueueHandle_t liveFrames = nullptr;
std::atomic<int> liveClient{-1};
std::atomic<bool> liveCaptureFault{false};
adc_continuous_handle_t adcHandle = nullptr;
i2s_chan_handle_t speakerHandle = nullptr;
i2s_chan_handle_t pdmHandle = nullptr;
adc_channel_t audioChannel;
TaskHandle_t recorderTask = nullptr;
std::atomic<bool> stopRequested{false};
std::atomic<bool> adcOverflow{false};
std::atomic<bool> signalEnabled{false};
std::atomic<bool> serialMeterEnabled{false};
std::atomic<bool> audioLowpassEnabled{false};
std::atomic<bool> signalActive{false};
std::atomic<uint32_t> signalLastPoll{0};
std::atomic<uint32_t> signalSequence{0};
std::atomic<uint16_t> signalRms{0};
std::atomic<uint16_t> signalPeak{0};
std::atomic<uint16_t> signalBias{0};
std::atomic<uint8_t> signalFault{0};
enum class CaptureState { idle, starting, recording };
std::atomic<CaptureState> captureState{CaptureState::idle};
std::atomic<uint32_t> recordingStartedAt{0};
enum class PlaybackState { idle, starting, playing };
std::atomic<PlaybackState> playbackState{PlaybackState::idle};
std::atomic<bool> playbackStopRequested{false};
std::atomic<uint8_t> speakerVolume{25};
std::atomic<uint8_t> speakerBoost{1};
TaskHandle_t playbackTask = nullptr;
char playbackPath[221] = {};
char playbackError[80] = {};
bool sdReady = false;
std::atomic<bool> recordingsPresent{false};
bool adcReady = false;
bool speakerReady = false;
bool wifiReady = false;
uint8_t wifiTestPhase = 0;
uint32_t wifiTestSince = 0;
portMUX_TYPE encoderMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint8_t encoderState = 0;
volatile int8_t encoderQuarterSteps = 0;
volatile int32_t encoderSteps = 0;
char recordingName[16] = {};
char lastError[80] = {};
uint8_t dmaBuffer[kDmaBufferSize];
uint8_t pcmBuffer[kDmaBufferSize / 2];

struct AudioLowpass {
  float input1 = 0;
  float input2 = 0;
  float output1 = 0;
  float output2 = 0;

  float process(float input) {
    float output = 0.1370154f * input + 0.2740308f * input1 + 0.1370154f * input2
                   + 0.7155086f * output1 - 0.2635703f * output2;
    input2 = input1;
    input1 = input;
    output2 = output1;
    output1 = output;
    return output;
  }
};

int16_t encodePcm(float centered, AudioLowpass &lowpass) {
  float filtered = lowpass.process(centered);
  float value = audioLowpassEnabled.load() ? filtered : centered;
  return static_cast<int16_t>(constrain(static_cast<int32_t>(value * 16), -32768, 32767));
}

String firmwareVersion() {
  return String(kFirmwareVersion) + "." + String(kFirmwareBuild);
}

void IRAM_ATTR onEncoderEdge() {
  static const DRAM_ATTR int8_t transitions[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  uint8_t state = (digitalRead(kEncoderClkGpio) << 1) | digitalRead(kEncoderDtGpio);
  portENTER_CRITICAL_ISR(&encoderMux);
  encoderQuarterSteps += transitions[(encoderState << 2) | state];
  encoderState = state;
  if (encoderQuarterSteps >= 4) {
    encoderSteps = encoderSteps + 1;
    encoderQuarterSteps = 0;
  } else if (encoderQuarterSteps <= -4) {
    encoderSteps = encoderSteps - 1;
    encoderQuarterSteps = 0;
  }
  portEXIT_CRITICAL_ISR(&encoderMux);
}

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
.filename{display:block;font-weight:bold;overflow-wrap:anywhere}.size{display:block;color:var(--muted);font-size:13px;margin-top:4px}audio{display:block;width:min(100%,400px);height:42px;margin-top:12px}.fileactions{display:flex;align-items:center;gap:12px;flex-wrap:wrap;justify-content:flex-end}
a{color:#12604f;text-decoration:underline;text-underline-offset:3px}#more{margin-top:20px}footer{color:var(--muted);border-top:1px solid var(--line);font-size:13px}
.signalbar{display:flex;align-items:center;justify-content:space-between;gap:16px;flex-wrap:wrap}.signalbar h2{margin:0}.signalvalues{color:var(--muted);margin:14px 0;font-variant-numeric:tabular-nums}
#signalChart{width:100%;height:200px;display:block;background:#f8faf7;border:1px solid var(--line)}.legend{display:flex;gap:20px;color:var(--muted);font-size:13px;margin-top:10px}.legend b{font-weight:normal}
.legend b:first-child{color:#12604f}.legend b:last-child{color:var(--accent)}
.livecontrols{display:flex;align-items:center;gap:12px;flex-wrap:wrap;margin-top:16px}.livecontrols label{font-size:14px;color:var(--muted)}.livecontrols input{width:120px;accent-color:#12604f}
.speaker-volume{width:min(100%,280px);margin:16px 0 20px}.speaker-volume label{display:block;font-size:14px;color:var(--muted);margin-bottom:6px}.speaker-volume input{width:100%;accent-color:#12604f}
.speaker-volume-steps{display:flex;justify-content:space-between;align-items:center}.speaker-volume-steps button{width:40px;height:40px;padding:0;font-size:22px}.speaker-volume-steps output{min-width:4ch;text-align:center;font-variant-numeric:tabular-nums}
.speaker-volume .boost-label{margin-top:16px}.speaker-volume select{font:inherit;padding:7px 10px;border:1px solid var(--line);background:#fff;color:var(--ink);width:100%}
.filtercontrol{display:flex;align-items:center;gap:10px;margin-top:20px;cursor:pointer}.filtercontrol input{width:18px;height:18px;accent-color:#12604f}
@media(max-width:560px){li{grid-template-columns:1fr}.fileactions{justify-content:flex-start}h1{font-size:30px}}
</style></head><body><header>RADIO RECORDER <span id="firmwareVersion">v?</span> <a href="/files">Fájlkezelő</a></header>
<main><h1>Felvétel</h1><p class="state" id="state">Kapcsolódás...</p><button id="toggle" disabled>Felvétel indítása</button> <button id="wifiTest" disabled>Wi-Fi zajteszt</button><p class="note" id="message"></p>
<label class="filtercontrol"><input id="lowpassToggle" type="checkbox">3,4 kHz-es zajszűrő</label>
<p class="note">Csak ellenőrzött, kis szintű tesztforrás. A rádiót még ne csatlakoztasd.</p></main>
<section><div class="signalbar"><h2 id="signalHeading">ADC jel</h2><button id="signalToggle" disabled>Mérés bekapcsolása</button></div>
<p id="signalValues" class="signalvalues">Kikapcsolva</p><div id="signalPlot" hidden><canvas id="signalChart" aria-label="ADC RMS és csúcs idősor" role="img"></canvas>
<div class="legend"><b>RMS</b><b>Csúcs</b><span>ADC egység / 30 s</span></div></div></section>
<section><div class="signalbar"><h2>Élő hang</h2><button id="liveToggle" disabled>Hallgatás indítása</button></div>
<div class="livecontrols"><label for="liveVolume">Hangerő</label><input id="liveVolume" type="range" min="0" max="100" value="50"><button id="liveTest">Hangpróba</button></div>
<p id="liveStatus" class="note">Kikapcsolva</p></section>
<section><div class="signalbar"><h2>Mentett felvételek</h2><button id="speakerStop" disabled>ESP-lejátszás leállítása</button></div><p id="speakerStatus" class="note">ESP hangszóró: kikapcsolva</p>
<div class="speaker-volume"><label for="speakerVolume">ESP hangszóró és próbajel hangereje</label><input id="speakerVolume" type="range" min="0" max="100" step="1" value="25"><div class="speaker-volume-steps"><button id="speakerDown" type="button" aria-label="Hangerő csökkentése 5%-kal" title="5%-kal halkabb">←</button><output id="speakerVolumeValue" for="speakerVolume">25%</output><button id="speakerUp" type="button" aria-label="Hangerő növelése 5%-kal" title="5%-kal hangosabb">→</button></div><label class="boost-label" for="speakerBoost">Lejátszási erősítés</label><select id="speakerBoost"><option value="1">1×</option><option value="2">2×</option><option value="4">4×</option></select></div>
<p id="empty">Betöltés...</p><ol id="files"></ol><button id="more" hidden>Továbbiak</button></section>
<footer>ESP32-S3 / microSD</footer>
<script>
const stateEl=document.querySelector('#state'),toggle=document.querySelector('#toggle'),wifiTest=document.querySelector('#wifiTest'),lowpassToggle=document.querySelector('#lowpassToggle'),message=document.querySelector('#message'),firmwareVersion=document.querySelector('#firmwareVersion');
const files=document.querySelector('#files'),empty=document.querySelector('#empty'),more=document.querySelector('#more');
const speakerStatus=document.querySelector('#speakerStatus'),speakerStop=document.querySelector('#speakerStop');
const speakerVolume=document.querySelector('#speakerVolume'),speakerVolumeValue=document.querySelector('#speakerVolumeValue'),speakerBoost=document.querySelector('#speakerBoost');
const signalToggle=document.querySelector('#signalToggle'),signalValues=document.querySelector('#signalValues');
const signalPlot=document.querySelector('#signalPlot'),signalChart=document.querySelector('#signalChart'),signalHeading=document.querySelector('#signalHeading');
const liveToggle=document.querySelector('#liveToggle'),liveStatus=document.querySelector('#liveStatus'),liveVolume=document.querySelector('#liveVolume'),liveTest=document.querySelector('#liveTest');
let currentState='',nextOffset=null,loaded=false,listing=false;
let meterRunning=false,meterGeneration=0,meterTimer=null,lastSignalSequence=-1;
let liveSocket=null,audioContext=null,audioGain=null,nextAudioTime=0;
let volumeQueued=null,volumeSending=false,boostUpdating=false;
const signalHistory=[];
async function sendSpeakerVolume(){
 if(volumeSending)return;
 volumeSending=true;
 while(volumeQueued!==null){
  const value=volumeQueued;volumeQueued=null;
  try{const response=await fetch('/api/play/volume?value='+value,{method:'POST'});if(!response.ok)throw Error()}
  catch{speakerStatus.textContent='A hangerő módosítása sikertelen.'}
 }
 volumeSending=false;
}
function setSpeakerVolume(value){
 const volume=Math.max(0,Math.min(100,value));
 speakerVolume.value=volume;speakerVolumeValue.textContent=volume+'%';
 volumeQueued=volume;sendSpeakerVolume();
}
speakerVolume.addEventListener('input',()=>setSpeakerVolume(Number(speakerVolume.value)));
document.querySelector('#speakerDown').addEventListener('click',()=>setSpeakerVolume(Number(speakerVolume.value)-5));
document.querySelector('#speakerUp').addEventListener('click',()=>setSpeakerVolume(Number(speakerVolume.value)+5));
speakerBoost.addEventListener('change',async()=>{
 boostUpdating=true;speakerBoost.disabled=true;
 let failed=false;
 try{const response=await fetch('/api/play/boost?value='+speakerBoost.value,{method:'POST'});if(!response.ok)throw Error()}
 catch{failed=true}
 boostUpdating=false;await refresh();
 if(failed)speakerStatus.textContent='Az erősítés módosítása sikertelen.';
});
async function playOnEsp(path){
 try{
  const response=await fetch('/api/play?path='+encodeURIComponent(path),{method:'POST'});
  if(!response.ok)throw Error(await response.text());
  await refresh();
 }catch(error){speakerStatus.textContent=error.message||'ESP-lejátszás nem indult.'}
}
speakerStop.addEventListener('click',async()=>{
 speakerStop.disabled=true;
 try{
  const response=await fetch('/api/play/stop',{method:'POST'});
  if(!response.ok)throw Error(await response.text());
 }catch(error){speakerStatus.textContent=error.message||'Leállítás sikertelen.'}
 await refresh();
});
function stopLive(){
 const socket=liveSocket;liveSocket=null;
 if(socket)socket.close();
 const context=audioContext;audioContext=null;audioGain=null;if(context)context.close().catch(()=>{});
 liveToggle.textContent='Hallgatás indítása';liveStatus.textContent='Kikapcsolva';
}
liveToggle.addEventListener('click',async()=>{
 if(liveSocket){stopLive();return}
 liveToggle.disabled=true;
 try{
  const Context=window.AudioContext||window.webkitAudioContext;if(!Context||!window.WebSocket)throw Error();
  audioContext=new Context({latencyHint:'interactive'});
  audioGain=audioContext.createGain();audioGain.gain.value=Number(liveVolume.value)/100;
  audioGain.connect(audioContext.destination);
  const unlock=audioContext.createBufferSource();unlock.buffer=audioContext.createBuffer(1,1,22050);
  unlock.connect(audioGain);unlock.start();
  await audioContext.resume();
  const socket=new WebSocket('ws://'+location.hostname+':81/');socket.binaryType='arraybuffer';liveSocket=socket;
  liveStatus.textContent='Kapcsolódás...';
  socket.onopen=()=>{if(socket!==liveSocket)return;liveToggle.textContent='Hallgatás leállítása';liveStatus.textContent='Várakozás hangra...'};
  socket.onmessage=event=>{
   if(socket!==liveSocket||!(event.data instanceof ArrayBuffer)||!audioContext)return;
   const view=new DataView(event.data),length=Math.floor(view.byteLength/2);if(!length)return;
   const buffer=audioContext.createBuffer(1,length,22050),channel=buffer.getChannelData(0);
  let peak=0;
  for(let index=0;index<length;index++){
   const value=view.getInt16(index*2,true)/32768;channel[index]=value;peak=Math.max(peak,Math.abs(value));
  }
  if(audioContext.state!=='running'){
   liveStatus.textContent='Lejátszás szünetel ('+audioContext.state+'), PCM csúcs: '+Math.round(peak*100)+'%';
   return;
  }
   const now=audioContext.currentTime;
  if(nextAudioTime<now+0.12||nextAudioTime>now+0.8)nextAudioTime=now+0.12;
   const source=audioContext.createBufferSource();source.buffer=buffer;source.connect(audioGain);source.start(nextAudioTime);
   nextAudioTime+=buffer.duration;
  liveStatus.textContent='Élő hang, PCM csúcs: '+Math.round(peak*100)+'%, kb. '+Math.round((nextAudioTime-now)*1000)+' ms puffer';
  };
  socket.onclose=()=>{if(socket!==liveSocket)return;stopLive();liveStatus.textContent='A kapcsolat lezárult.'};
  socket.onerror=()=>{if(socket!==liveSocket)return;stopLive();liveStatus.textContent='A hangkapcsolat nem elérhető.'};
 }catch{stopLive();liveStatus.textContent='A böngésző nem tudja elindítani a hangot.'}
 liveToggle.disabled=false;
});
liveVolume.addEventListener('input',()=>{if(audioGain)audioGain.gain.value=Number(liveVolume.value)/100});
liveTest.addEventListener('click',async()=>{
 const Context=window.AudioContext||window.webkitAudioContext;
 if(!Context){liveStatus.textContent='Hangpróba nem támogatott.';return}
 const context=audioContext||new Context({latencyHint:'interactive'});
 try{
  const tone=context.createOscillator(),gain=context.createGain();
  tone.frequency.value=440;gain.gain.value=Number(liveVolume.value)/100*0.15;
  tone.connect(gain);gain.connect(context.destination);
  tone.onended=()=>{tone.disconnect();gain.disconnect();if(context!==audioContext)context.close().catch(()=>{})};
  tone.start();tone.stop(context.currentTime+0.4);
  await context.resume();
  liveStatus.textContent='Hangpróba lejátszva.';
 }catch{if(context!==audioContext)context.close().catch(()=>{});liveStatus.textContent='A hangpróba nem sikerült.'}
});
addEventListener('pagehide',stopLive);
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
  const actions=document.createElement('div');actions.className='fileactions';
  const espPlay=document.createElement('button');espPlay.type='button';espPlay.dataset.espPlay='/'+item.name;
  espPlay.textContent='ESP-n lejátszás';espPlay.title='Lejátszás az ESP hangszóróján';espPlay.addEventListener('click',()=>playOnEsp('/'+item.name));
  const download=document.createElement('a');download.href=audio.src+'&download=1';download.download=item.name;download.textContent='Letöltés';
  actions.append(espPlay,download);row.append(info,actions);files.append(row);
 }
 nextOffset=data.next;more.hidden=nextOffset===null;empty.hidden=files.children.length>0;empty.textContent='Nincs mentett felvétel.';loaded=true;
 }finally{listing=false}
}
async function refresh(){
 try{
  const response=await fetch('/api/status',{cache:'no-store'});if(!response.ok)throw Error();
  const data=await response.json();
  firmwareVersion.textContent='v'+data.version;
  lowpassToggle.checked=data.lowpass;
  signalHeading.textContent='GPIO'+data.audioGpio+' jel';
  signalChart.setAttribute('aria-label','GPIO'+data.audioGpio+' RMS és csúcs idősor');
  const busy=data.state!=='idle';
  const playing=data.playback!=='idle';
  stateEl.textContent=!data.ready?'SD vagy ADC nem elérhető':busy?(data.state==='starting'?'Felvétel indul...':'Felvétel folyamatban'):(data.hasRecordings?'Van mentett felvétel':'Nincs mentett felvétel');
  toggle.textContent=busy?'Felvétel leállítása':'Felvétel indítása';toggle.classList.toggle('recording',busy);
  toggle.disabled=!data.ready||data.state==='starting'||(busy&&data.stopping)||playing;
  wifiTest.disabled=!data.ready||busy||playing||!!liveSocket;
  speakerStop.disabled=!playing;
  speakerStatus.textContent=playing?'ESP hangszóró: '+(data.playback==='starting'?'indul...':data.playbackPath):data.playbackError||'ESP hangszóró: kikapcsolva';
  if(!volumeSending){speakerVolume.value=data.speakerVolume;speakerVolumeValue.textContent=data.speakerVolume+'%'}
  speakerBoost.disabled=!data.speakerReady||boostUpdating;
  if(!boostUpdating)speakerBoost.value=String(data.speakerBoost);
  signalToggle.disabled=!data.meterReady&&!meterRunning;
  liveToggle.disabled=(!data.liveReady||playing)&&!liveSocket;
  message.textContent=data.error||'';
  if(!busy&&(!loaded||currentState!=='idle'))await list(true);
  for(const button of files.querySelectorAll('[data-esp-play]'))button.disabled=busy||playing||!data.speakerReady||!!liveSocket;
  currentState=data.state;
 }catch{stateEl.textContent='A kapcsolat megszakadt';toggle.disabled=true}
}
toggle.addEventListener('click',async()=>{
 toggle.disabled=true;
 try{const response=await fetch(currentState==='idle'?'/api/start':'/api/stop',{method:'POST'});if(!response.ok)message.textContent=await response.text();}
 catch{message.textContent='A kérés nem sikerült.'}
 await refresh();
});
lowpassToggle.addEventListener('change',async()=>{
 const enabled=lowpassToggle.checked;lowpassToggle.disabled=true;
 try{
  const response=await fetch(enabled?'/api/filter/on':'/api/filter/off',{method:'POST'});
  if(!response.ok)throw Error();
 }catch{lowpassToggle.checked=!enabled;message.textContent='A zajszűrő kapcsolása sikertelen.'}
 await refresh();lowpassToggle.disabled=false;
});
wifiTest.addEventListener('click',async()=>{
 wifiTest.disabled=true;
 try{const response=await fetch('/api/wifi-test',{method:'POST'});if(!response.ok)message.textContent=await response.text();}
 catch{message.textContent='A zajteszt nem indult el.'}
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
.name{font-weight:700;overflow-wrap:anywhere}.meta{color:var(--muted);font-size:13px;margin:4px 0 0}.actions{display:flex;align-items:center;gap:12px;flex-shrink:0;flex-wrap:wrap;justify-content:flex-end}audio{width:min(100%,400px);height:40px;display:block;margin-top:12px}
.speaker-volume{width:min(100%,280px);margin:16px 0 20px}.speaker-volume label{display:block;font-size:14px;color:var(--muted);margin-bottom:6px}.speaker-volume input{width:100%;accent-color:#12604f}
.speaker-volume-steps{display:flex;justify-content:space-between;align-items:center}.speaker-volume-steps button{width:40px;height:40px;padding:0;font-size:22px}.speaker-volume-steps output{min-width:4ch;text-align:center;font-variant-numeric:tabular-nums}
.speaker-volume .boost-label{margin-top:16px}.speaker-volume select{font:inherit;padding:7px 10px;border:1px solid var(--line);background:#fff;color:var(--ink);width:100%}
#message{color:var(--muted);margin:20px 0;line-height:1.4}#more{margin-top:22px}footer{border-top:1px solid var(--line);color:var(--muted);font-size:13px}
@media(max-width:520px){.items li{display:block}.actions{margin-top:12px;justify-content:flex-start}.toolbar{align-items:flex-start;flex-direction:column}h1{font-size:29px}}
</style></head><body><header>RADIO RECORDER <span id="firmwareVersion">v?</span> <a href="/">Felvétel</a></header>
<main><h1>Fájlkezelő</h1><ol id="crumbs" class="crumbs"></ol>
<div class="toolbar"><strong id="location">microSD</strong><button id="reload" title="Lista frissítése">Frissítés</button></div>
<div class="toolbar"><span id="speakerStatus">ESP hangszóró: kikapcsolva</span><button id="speakerStop" disabled>ESP-lejátszás leállítása</button></div>
<div class="speaker-volume"><label for="speakerVolume">ESP hangszóró és próbajel hangereje</label><input id="speakerVolume" type="range" min="0" max="100" step="1" value="25"><div class="speaker-volume-steps"><button id="speakerDown" type="button" aria-label="Hangerő csökkentése 5%-kal" title="5%-kal halkabb">←</button><output id="speakerVolumeValue" for="speakerVolume">25%</output><button id="speakerUp" type="button" aria-label="Hangerő növelése 5%-kal" title="5%-kal hangosabb">→</button></div><label class="boost-label" for="speakerBoost">Lejátszási erősítés</label><select id="speakerBoost"><option value="1">1×</option><option value="2">2×</option><option value="4">4×</option></select></div>
<p id="message">Betöltés...</p><ol id="items" class="items"></ol><button id="more" hidden>Továbbiak</button></main>
<footer>Lejátszás a böngésző által támogatott hangformátumokkal.</footer>
<script>
const folder=new URLSearchParams(location.search).get('dir')||'/';
fetch('/api/status',{cache:'no-store'}).then(response=>response.json()).then(data=>{
 document.querySelector('#firmwareVersion').textContent='v'+data.version;
}).catch(()=>{});
const listEl=document.querySelector('#items'),message=document.querySelector('#message'),more=document.querySelector('#more');
const speakerStatus=document.querySelector('#speakerStatus'),speakerStop=document.querySelector('#speakerStop');
const speakerVolume=document.querySelector('#speakerVolume'),speakerVolumeValue=document.querySelector('#speakerVolumeValue'),speakerBoost=document.querySelector('#speakerBoost');
let next=0,loading=false;
let volumeQueued=null,volumeSending=false,boostUpdating=false;
const sound=name=>/\.(wav|mp3|m4a|mp4|ogg|oga|opus|flac|aac|webm)$/i.test(name);
const link=path=>'/files?dir='+encodeURIComponent(path);
async function sendSpeakerVolume(){
 if(volumeSending)return;
 volumeSending=true;
 while(volumeQueued!==null){
  const value=volumeQueued;volumeQueued=null;
  try{const response=await fetch('/api/play/volume?value='+value,{method:'POST'});if(!response.ok)throw Error()}
  catch{speakerStatus.textContent='A hangerő módosítása sikertelen.'}
 }
 volumeSending=false;
}
function setSpeakerVolume(value){
 const volume=Math.max(0,Math.min(100,value));
 speakerVolume.value=volume;speakerVolumeValue.textContent=volume+'%';
 volumeQueued=volume;sendSpeakerVolume();
}
speakerVolume.addEventListener('input',()=>setSpeakerVolume(Number(speakerVolume.value)));
document.querySelector('#speakerDown').addEventListener('click',()=>setSpeakerVolume(Number(speakerVolume.value)-5));
document.querySelector('#speakerUp').addEventListener('click',()=>setSpeakerVolume(Number(speakerVolume.value)+5));
speakerBoost.addEventListener('change',async()=>{
 boostUpdating=true;speakerBoost.disabled=true;
 let failed=false;
 try{const response=await fetch('/api/play/boost?value='+speakerBoost.value,{method:'POST'});if(!response.ok)throw Error()}
 catch{failed=true}
 boostUpdating=false;await refreshSpeaker();
 if(failed)speakerStatus.textContent='Az erősítés módosítása sikertelen.';
});
async function refreshSpeaker(){
 try{
  const response=await fetch('/api/status',{cache:'no-store'});if(!response.ok)throw Error();
  const data=await response.json(),playing=data.playback!=='idle';
  speakerStop.disabled=!playing;
  speakerStatus.textContent=playing?'ESP hangszóró: '+(data.playback==='starting'?'indul...':data.playbackPath):data.playbackError||'ESP hangszóró: kikapcsolva';
  if(!volumeSending){speakerVolume.value=data.speakerVolume;speakerVolumeValue.textContent=data.speakerVolume+'%'}
  speakerBoost.disabled=!data.speakerReady||boostUpdating;
  if(!boostUpdating)speakerBoost.value=String(data.speakerBoost);
  for(const button of listEl.querySelectorAll('[data-esp-play]'))button.disabled=playing||data.state!=='idle'||!data.speakerReady;
 }catch{speakerStatus.textContent='ESP-lejátszás állapota nem elérhető.'}
}
async function playOnEsp(path){
 try{
  const response=await fetch('/api/play?path='+encodeURIComponent(path),{method:'POST'});
  if(!response.ok)throw Error(await response.text());
  await refreshSpeaker();
 }catch(error){speakerStatus.textContent=error.message||'ESP-lejátszás nem indult.'}
}
speakerStop.addEventListener('click',async()=>{
 speakerStop.disabled=true;
 try{
  const response=await fetch('/api/play/stop',{method:'POST'});
  if(!response.ok)throw Error(await response.text());
 }catch(error){speakerStatus.textContent=error.message||'Leállítás sikertelen.'}
 await refreshSpeaker();
});
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
  if(/\.wav$/i.test(item.name)){
   const espPlay=document.createElement('button');espPlay.type='button';espPlay.dataset.espPlay=item.path;
   espPlay.textContent='ESP-n lejátszás';espPlay.title='Lejátszás az ESP hangszóróján';espPlay.addEventListener('click',()=>playOnEsp(item.path));
   actions.append(espPlay);
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
  const data=await response.json();data.items.forEach(render);next=data.next;refreshSpeaker();
  more.hidden=next===null;message.textContent=listEl.children.length?'':'Üres mappa.';
 }catch(error){message.textContent=error.message;more.hidden=true}
 finally{loading=false}
}
document.querySelector('#reload').addEventListener('click',()=>load(true));
more.addEventListener('click',()=>load());breadcrumbs();load();setInterval(refreshSpeaker,1500);
</script></body></html>)FILES";

void put16(uint8_t *destination, uint16_t value) {
  destination[0] = value & 0xff;
  destination[1] = value >> 8;
}

void queueLiveAudio(const uint8_t *samples, size_t length) {
  if (liveClient.load() < 0 || liveFrames == nullptr) return;
  for (size_t offset = 0; offset < length; offset += kLiveFrameBytes) {
    AudioFrame frame = {};
    frame.length = min(kLiveFrameBytes, length - offset);
    memcpy(frame.samples, samples + offset, frame.length);
    xQueueSend(liveFrames, &frame, 0);
  }
}

void onAudioSocket(uint8_t client, WStype_t event, uint8_t *, size_t) {
  if (event == WStype_CONNECTED) {
    if (!adcReady || recorderTask == nullptr || liveFrames == nullptr || liveClient.load() >= 0 ||
        playbackState.load() != PlaybackState::idle) {
      audioSocket.disconnect(client);
      return;
    }
    xQueueReset(liveFrames);
    liveCaptureFault.store(false);
    liveClient.store(client);
    xTaskNotifyGive(recorderTask);
    Serial.println("Live audio connected");
  } else if (event == WStype_DISCONNECTED && liveClient.load() == client) {
    liveClient.store(-1);
    xQueueReset(liveFrames);
    Serial.println("Live audio disconnected");
  }
}

void sendLiveFrames() {
  int client = liveClient.load();
  if (client < 0 || liveFrames == nullptr) return;
  if (liveCaptureFault.exchange(false)) {
    audioSocket.disconnect(client);
    return;
  }
  AudioFrame frame;
  if (xQueueReceive(liveFrames, &frame, 0) == pdTRUE && !audioSocket.sendBIN(client, frame.samples, frame.length)) {
    audioSocket.disconnect(client);
  }
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
  if (serialMeterEnabled.load()) return true;
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
  if (!sdReady || !adcReady || recorderTask == nullptr || captureState.load() != CaptureState::idle ||
      playbackState.load() != PlaybackState::idle) return false;
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

void pollEncoderRotation() {
  static uint32_t nextActionAt = 0;
  portENTER_CRITICAL(&encoderMux);
  int32_t steps = encoderSteps;
  encoderSteps = 0;
  portEXIT_CRITICAL(&encoderMux);
  if (!steps || static_cast<int32_t>(millis() - nextActionAt) < 0) return;
  nextActionAt = millis() + 300;
  if (steps > 0) {
    if (wifiTestPhase != 0) {
      Serial.println("Zajteszt alatt a felvetel nem kapcsolhato");
    } else if (captureState.load() == CaptureState::idle) {
      Serial.println(startRecording() ? String("Felvetel indul: ") + recordingName : String("Felvetel nem indithato: ") + lastError);
    } else if (stopRequested.load()) {
      Serial.println("Felvetel leallitasa folyamatban");
    } else {
      Serial.println(stopRecording() ? "Felvetel leallitasa kerese" : "Felvetel nem allithato le");
    }
  } else {
    bool enabled = !serialMeterEnabled.load();
    if (enabled && (!adcReady || recorderTask == nullptr)) {
      Serial.println("ADC meres nem elerheto");
      return;
    }
    serialMeterEnabled.store(enabled);
    if (enabled) {
      signalFault.store(0);
      xTaskNotifyGive(recorderTask);
    }
    Serial.println(enabled ? "Soros ADC meres bekapcsolva" : "Soros ADC meres kikapcsolva");
  }
}

void pollSerialMeter() {
  static bool wasEnabled = false;
  static uint32_t lastSequence = 0;
  static uint32_t lastPrintAt = 0;
  if (!serialMeterEnabled.load()) {
    wasEnabled = false;
    return;
  }
  if (!wasEnabled) {
    lastSequence = signalSequence.load();
    lastPrintAt = millis() - 500;
    wasEnabled = true;
  }
  if (millis() - lastPrintAt < 500) return;
  uint8_t fault = signalFault.load();
  if (fault) {
    Serial.printf("ADC meresi hiba (%u)\n", fault);
    serialMeterEnabled.store(false);
    return;
  }
  uint32_t sequence = signalSequence.load();
  if (sequence == lastSequence) return;
  lastSequence = sequence;
  lastPrintAt = millis();
  char recordingLabel[64] = {};
  if (captureState.load() == CaptureState::recording) {
    uint32_t elapsedMs = lastPrintAt - recordingStartedAt.load();
    snprintf(recordingLabel, sizeof(recordingLabel), "REC %s | t=%lu.%03lu s | ", recordingName,
             static_cast<unsigned long>(elapsedMs / 1000), static_cast<unsigned long>(elapsedMs % 1000));
  }
  Serial.printf("%sGPIO%d RMS: %u | Csucs: %u | DC-kozep: %u (ADC)\n", recordingLabel, kAudioGpio,
                signalRms.load(), signalPeak.load(), signalBias.load());
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

uint16_t wav16(const uint8_t *bytes) {
  return static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(bytes[1]) << 8;
}

uint32_t wav32(const uint8_t *bytes) {
  return static_cast<uint32_t>(wav16(bytes)) | static_cast<uint32_t>(wav16(bytes + 2)) << 16;
}

bool findPcmData(File &file, uint32_t &remaining) {
  uint8_t header[16];
  if (!file.seek(0) || file.read(header, 12) != 12 || memcmp(header, "RIFF", 4) != 0 ||
      memcmp(header + 8, "WAVE", 4) != 0) return false;
  uint64_t riffEnd = static_cast<uint64_t>(wav32(header + 4)) + 8;
  if (riffEnd > file.size() || riffEnd > UINT32_MAX || riffEnd < 44) return false;
  uint32_t dataOffset = 0;
  uint32_t dataSize = 0;
  bool pcmFormat = false;
  while (static_cast<uint64_t>(file.position()) + 8 <= riffEnd) {
    if (file.read(header, 8) != 8) return false;
    uint32_t size = wav32(header + 4);
    uint64_t end = static_cast<uint64_t>(file.position()) + size + (size & 1);
    if (end > riffEnd) return false;
    if (memcmp(header, "fmt ", 4) == 0 && size >= 16) {
      if (file.read(header, 16) != 16) return false;
      pcmFormat = wav16(header) == 1 && wav16(header + 2) == 1 && wav32(header + 4) == kSampleRate &&
                  wav32(header + 8) == kSampleRate * 2 && wav16(header + 12) == 2 && wav16(header + 14) == 16;
    } else if (memcmp(header, "data", 4) == 0) {
      dataOffset = file.position();
      dataSize = size;
    }
    if (!file.seek(static_cast<uint32_t>(end))) return false;
  }
  if (!pcmFormat || !dataOffset || !dataSize || (dataSize & 1)) return false;
  remaining = dataSize;
  return file.seek(dataOffset);
}

void speakerLoop(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (playbackState.load() != PlaybackState::starting) continue;
    File file = SD_MMC.open(playbackPath, FILE_READ);
    uint32_t remaining = 0;
    bool enabled = false;
    bool pdmEnabled = false;
    if (!file || !findPcmData(file, remaining)) {
      snprintf(playbackError, sizeof(playbackError), "Cannot read PCM WAV");
    } else if (!playbackStopRequested.load()) {
      enabled = i2s_channel_enable(speakerHandle) == ESP_OK;
      if (!enabled) {
        snprintf(playbackError, sizeof(playbackError), "Cannot start speaker");
      } else {
        pdmEnabled = pdmHandle && i2s_channel_enable(pdmHandle) == ESP_OK;
        if (pdmHandle && !pdmEnabled) Serial.println("Cannot start PDM test output");
        playbackState.store(PlaybackState::playing);
        uint8_t input[512];
        int16_t output[512];
        int16_t pdmOutput[256];
        float appliedGain = 0;
        while (remaining && !playbackStopRequested.load()) {
          size_t count = file.read(input, min(static_cast<uint32_t>(sizeof(input)), remaining));
          if (!count || (count & 1)) {
            snprintf(playbackError, sizeof(playbackError), "WAV read failed");
            break;
          }
          remaining -= count;
          int32_t peak = 0;
          for (size_t index = 0; index < count / 2; ++index) {
            int32_t sample = static_cast<int16_t>(wav16(input + index * 2));
            peak = max(peak, abs(sample));
          }
          float requestedGain = speakerVolume.load() * speakerBoost.load() / 100.0f;
          float limitedGain = peak ? min(requestedGain, 32767.0f / peak) : requestedGain;
          appliedGain = min(limitedGain, appliedGain + 0.1f);
          for (size_t index = 0; index < count / 2; ++index) {
            int16_t sample = constrain(static_cast<int32_t>(static_cast<int16_t>(wav16(input + index * 2)) * appliedGain),
                                       -32767, 32767);
            output[index * 2] = sample;
            output[index * 2 + 1] = sample;
            pdmOutput[index] = sample;
          }
          size_t written = 0;
          if (i2s_channel_write(speakerHandle, output, count * 2, &written, 1000) != ESP_OK || written != count * 2) {
            snprintf(playbackError, sizeof(playbackError), "Speaker output failed");
            break;
          }
          if (pdmEnabled &&
              (i2s_channel_write(pdmHandle, pdmOutput, count, &written, 1000) != ESP_OK || written != count)) {
            Serial.println("PDM test output failed");
            i2s_channel_disable(pdmHandle);
            pdmEnabled = false;
          }
        }
        if (!remaining && !playbackStopRequested.load() && !playbackError[0]) {
          int16_t silence[512] = {};
          for (int index = 0; index < 4; ++index) {
            size_t written = 0;
            if (i2s_channel_write(speakerHandle, silence, sizeof(silence), &written, 1000) != ESP_OK) break;
          }
          if (pdmEnabled) {
            int16_t pdmSilence[256] = {};
            for (int index = 0; index < 4; ++index) {
              size_t written = 0;
              if (i2s_channel_write(pdmHandle, pdmSilence, sizeof(pdmSilence), &written, 1000) != ESP_OK) break;
            }
          }
          vTaskDelay(pdMS_TO_TICKS(30));
        }
      }
    }
    if (pdmEnabled) i2s_channel_disable(pdmHandle);
    if (enabled) i2s_channel_disable(speakerHandle);
    if (file) file.close();
    playbackState.store(PlaybackState::idle);
  }
}

void sendStatus() {
  CaptureState state = captureState.load();
  bool ready = sdReady && adcReady && recorderTask != nullptr;
  String json = "{\"ready\":" + String(ready ? "true" : "false");
  json += ",\"version\":\"" + firmwareVersion() + "\"";
  json += ",\"audioGpio\":" + String(kAudioGpio);
  json += ",\"lowpass\":" + String(audioLowpassEnabled.load() ? "true" : "false");
  json += ",\"meterReady\":" + String(adcReady && recorderTask ? "true" : "false");
  json += ",\"liveReady\":" + String(adcReady && recorderTask && liveFrames && wifiReady ? "true" : "false");
  json += ",\"state\":\"" + String(state == CaptureState::idle ? "idle" : state == CaptureState::starting ? "starting" : "recording") + "\"";
  PlaybackState playback = playbackState.load();
  json += ",\"speakerReady\":" + String(speakerReady && playbackTask ? "true" : "false");
  json += ",\"speakerVolume\":" + String(speakerVolume.load());
  json += ",\"speakerBoost\":" + String(speakerBoost.load());
  json += ",\"playback\":\"" + String(playback == PlaybackState::idle ? "idle" : playback == PlaybackState::starting ? "starting" : "playing") + "\"";
  json += ",\"playbackPath\":\"" + String(playback == PlaybackState::idle ? "" : playbackPath) + "\"";
  json += ",\"playbackError\":\"" + String(playbackError) + "\"";
  json += ",\"stopping\":" + String(stopRequested.load() ? "true" : "false");
  json += ",\"hasRecordings\":" + String(ready && state == CaptureState::idle && recordingsPresent.load() ? "true" : "false");
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
  uint32_t total = 0;
  File entry;
  while ((entry = directory.openNextFile())) {
    if (!entry.isDirectory() && isRecordingFile(fileName(entry)) && entry.size() >= 44) ++total;
    entry.close();
  }
  directory.close();

  const uint32_t end = offset < total ? total - offset : 0;
  const uint32_t start = end > 20 ? end - 20 : 0;
  String names[20];
  size_t sizes[20];
  uint32_t index = 0;
  uint32_t shown = 0;
  if (start < end) {
    directory = SD_MMC.open("/");
    if (!directory) {
      server.send(500, "text/plain", "Cannot read SD card");
      return;
    }
    while ((entry = directory.openNextFile())) {
      String name = fileName(entry);
      if (!entry.isDirectory() && isRecordingFile(name) && entry.size() >= 44) {
        if (index >= start && index < end) {
          names[shown] = name;
          sizes[shown++] = entry.size();
        }
        if (++index >= end) {
          entry.close();
          break;
        }
      }
      entry.close();
    }
    directory.close();
  }
  String json = "{\"files\":[";
  for (uint32_t item = shown; item > 0; --item) {
    if (item < shown) json += ',';
    json += "{\"name\":\"" + names[item - 1] + "\",\"bytes\":" + String(sizes[item - 1]) + "}";
  }
  json += offset + shown < total ? "],\"next\":" + String(offset + shown) + "}" : "],\"next\":null}";
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

void startSpeakerPlayback() {
  if (!sdReady || !speakerReady || !playbackTask) {
    server.send(503, "text/plain", "Speaker or SD unavailable");
    return;
  }
  if (captureState.load() != CaptureState::idle || playbackState.load() != PlaybackState::idle || wifiTestPhase ||
      liveClient.load() >= 0) {
    server.send(409, "text/plain", "Stop recording, playback or live audio first");
    return;
  }
  String path = server.arg("path");
  String lowerPath = path;
  lowerPath.toLowerCase();
  if (!safeSdPath(path) || path == "/" || !lowerPath.endsWith(".wav")) {
    server.send(400, "text/plain", "Invalid WAV path");
    return;
  }
  File file = SD_MMC.open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    server.send(404, "text/plain", "WAV not found");
    return;
  }
  uint32_t remaining = 0;
  bool supported = findPcmData(file, remaining);
  file.close();
  if (!supported) {
    server.send(415, "text/plain", "Only mono 16-bit 22050 Hz PCM WAV is supported");
    return;
  }
  path.toCharArray(playbackPath, sizeof(playbackPath));
  playbackError[0] = '\0';
  playbackStopRequested.store(false);
  playbackState.store(PlaybackState::starting);
  xTaskNotifyGive(playbackTask);
  server.send(200, "text/plain", "Playing on ESP speaker");
}

void initWeb() {
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP("RadioRecorder")) {
    Serial.println("Cannot start Wi-Fi AP");
    return;
  }
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", kPage);
  });
  server.on("/files", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html; charset=utf-8", kFilesPage);
  });
  server.on("/api/status", HTTP_GET, sendStatus);
  server.on("/api/play", HTTP_POST, startSpeakerPlayback);
  server.on("/api/play/volume", HTTP_POST, []() {
    String value = server.arg("value");
    if (value.isEmpty() || value.length() > 3) {
      server.send(400, "text/plain", "Volume must be 0-100");
      return;
    }
    for (char digit : value) {
      if (!isDigit(digit)) {
        server.send(400, "text/plain", "Volume must be 0-100");
        return;
      }
    }
    int volume = value.toInt();
    if (volume > 100) {
      server.send(400, "text/plain", "Volume must be 0-100");
      return;
    }
    speakerVolume.store(volume);
    server.send(200, "text/plain", value);
  });
  server.on("/api/play/boost", HTTP_POST, []() {
    String value = server.arg("value");
    if (value != "1" && value != "2" && value != "4") {
      server.send(400, "text/plain", "Boost must be 1, 2 or 4");
      return;
    }
    speakerBoost.store(value.toInt());
    server.send(200, "text/plain", value);
  });
  server.on("/api/play/stop", HTTP_POST, []() {
    if (playbackState.load() == PlaybackState::idle) {
      server.send(409, "text/plain", "Speaker is idle");
      return;
    }
    playbackStopRequested.store(true);
    server.send(200, "text/plain", "Stopping speaker");
  });
  server.on("/api/filter/on", HTTP_POST, []() {
    audioLowpassEnabled.store(true);
    server.send(200, "text/plain", "Low-pass enabled");
  });
  server.on("/api/filter/off", HTTP_POST, []() {
    audioLowpassEnabled.store(false);
    server.send(200, "text/plain", "Low-pass disabled");
  });
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
  server.on("/api/wifi-test", HTTP_POST, []() {
    if (liveClient.load() >= 0 || wifiTestPhase != 0 || !startRecording()) {
      server.send(409, "text/plain", "Stop live audio and other recordings first");
      return;
    }
    wifiTestPhase = 1;
    server.send(200, "text/plain", recordingName);
  });
  server.on("/api/stop", HTTP_POST, []() {
    if (stopRecording()) server.send(200, "text/plain", "Stopping");
    else server.send(409, "text/plain", "Not recording");
  });
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  const char *headers[] = {"Range"};
  server.collectHeaders(headers, 1);
  server.begin();
  audioSocket.onEvent(onAudioSocket);
  audioSocket.begin();
  wifiReady = true;
}

void toggleWifi() {
  if (wifiTestPhase != 0) {
    Serial.println("Wi-Fi zajteszt alatt nem kapcsolhato");
    return;
  }
  if (wifiReady) {
    server.close();
    audioSocket.close();
    liveClient.store(-1);
    if (liveFrames) xQueueReset(liveFrames);
    if (!WiFi.mode(WIFI_OFF)) {
      server.begin();
      audioSocket.begin();
      Serial.println("Wi-Fi kikapcsolasa sikertelen");
      return;
    }
    wifiReady = false;
    Serial.println("Wi-Fi kikapcsolva; a felvetel folytatodik");
  } else if (WiFi.mode(WIFI_AP) && WiFi.softAP("RadioRecorder")) {
    server.begin();
    audioSocket.begin();
    wifiReady = true;
    Serial.print("Wi-Fi bekapcsolva: http://");
    Serial.println(WiFi.softAPIP());
  } else {
    WiFi.mode(WIFI_OFF);
    Serial.println("Wi-Fi bekapcsolasa sikertelen");
  }
}

void pollEncoderButton() {
  static bool lastReading = HIGH;
  static bool stable = HIGH;
  static uint32_t changedAt = 0;
  bool reading = digitalRead(kEncoderSwGpio);
  if (reading != lastReading) changedAt = millis();
  lastReading = reading;
  if (reading != stable && millis() - changedAt >= 30) {
    stable = reading;
    if (stable == LOW) toggleWifi();
  }
}

void printSetupStatus() {
  Serial.printf("Firmware: v%s\n", firmwareVersion().c_str());
  Serial.printf("3.4 kHz low-pass: %s (f = toggle)\n", audioLowpassEnabled.load() ? "on" : "off");
  Serial.printf("SD card: %s\n", sdReady ? "ready" : "unavailable");
  Serial.printf("ADC GPIO%d: %s\n", kAudioGpio, adcReady ? "ready" : "unavailable");
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
    liveCaptureFault.store(true);
    return;
  }
  signalActive.store(true);
  SignalWindow window;
  AudioLowpass lowpass;
  float dcLevel = 2048.0f;
  while ((signalRequested() || liveClient.load() >= 0) && captureState.load() == CaptureState::idle) {
    if (adcOverflow.load(std::memory_order_relaxed)) {
      signalFault.store(2);
      signalEnabled.store(false);
      liveCaptureFault.store(true);
      break;
    }
    uint32_t length = 0;
    esp_err_t result = adc_continuous_read(adcHandle, dmaBuffer, sizeof(dmaBuffer), &length, 100);
    if (result == ESP_ERR_TIMEOUT) continue;
    if (result != ESP_OK || length % SOC_ADC_DIGI_RESULT_BYTES != 0) {
      signalFault.store(3);
      signalEnabled.store(false);
      liveCaptureFault.store(true);
      break;
    }
    uint32_t validSamples = 0;
    size_t pcmLength = 0;
    bool publishSignal = signalRequested();
    for (uint32_t offset = 0; offset < length; offset += SOC_ADC_DIGI_RESULT_BYTES) {
      adc_digi_output_data_t sample;
      memcpy(&sample, dmaBuffer + offset, sizeof(sample));
      if (sample.type2.unit != ADC_UNIT_1 || sample.type2.channel != audioChannel) continue;
      ++validSamples;
      dcLevel += (static_cast<float>(sample.type2.data) - dcLevel) / 1024.0f;
      float centered = static_cast<float>(sample.type2.data) - dcLevel;
      if (publishSignal) addSignalSample(window, sample.type2.data, static_cast<int32_t>(centered));
      if (liveClient.load() >= 0) {
        put16(pcmBuffer + pcmLength, static_cast<uint16_t>(encodePcm(centered, lowpass)));
        pcmLength += 2;
      }
    }
    if (!validSamples) {
      signalFault.store(3);
      signalEnabled.store(false);
      liveCaptureFault.store(true);
      break;
    }
    if (pcmLength) queueLiveAudio(pcmBuffer, pcmLength);
  }
  adc_continuous_stop(adcHandle);
  signalActive.store(false);
}

void recordLoop(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (captureState.load() == CaptureState::idle && (signalRequested() || liveClient.load() >= 0)) monitorIdle();
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
      recordingStartedAt.store(millis());
      captureState.store(CaptureState::recording);
      Serial.printf("Recording started: %s\n", recordingName);
      float dcLevel = 2048.0f;
      SignalWindow window;
      AudioLowpass lowpass;
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
          if (sample.type2.unit != ADC_UNIT_1 || sample.type2.channel != audioChannel) continue;
          dcLevel += (static_cast<float>(sample.type2.data) - dcLevel) / 1024.0f;
          float centered = static_cast<float>(sample.type2.data) - dcLevel;
          if (publishSignal) addSignalSample(window, sample.type2.data, static_cast<int32_t>(centered));
          put16(pcmBuffer + pcmLength, static_cast<uint16_t>(encodePcm(centered, lowpass)));
          pcmLength += 2;
        }
        if (pcmLength == 0) {
          snprintf(lastError, sizeof(lastError), "ADC returned no valid samples");
          break;
        }
        queueLiveAudio(pcmBuffer, pcmLength);
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
    if (valid) {
      recordingsPresent.store(true);
      Serial.printf("Recording stopped: %s (%lu audio bytes)\n", recordingName, static_cast<unsigned long>(dataBytes));
    }
    else Serial.printf("Recording failed: %s\n", recordingName);
    if (lastError[0]) Serial.printf("Recording error: %s\n", lastError);
    captureState.store(CaptureState::idle);
    if (signalRequested() || liveClient.load() >= 0) xTaskNotifyGive(recorderTask);
  }
}

bool initAdc() {
  adc_unit_t unit;
  if (adc_continuous_io_to_channel(kAudioGpio, &unit, &audioChannel) != ESP_OK || unit != ADC_UNIT_1) return false;
  adc_continuous_handle_cfg_t handleConfig = {};
  handleConfig.max_store_buf_size = 32768;
  handleConfig.conv_frame_size = 1024;
  if (adc_continuous_new_handle(&handleConfig, &adcHandle) != ESP_OK) return false;

  adc_digi_pattern_config_t pattern = {};
  pattern.atten = ADC_ATTEN_DB_12;
  pattern.channel = audioChannel;
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

bool initSpeaker() {
  i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  if (i2s_new_channel(&channel, &speakerHandle, nullptr) != ESP_OK) return false;
  i2s_std_config_t config = {};
  config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
  config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  config.gpio_cfg.bclk = kSpeakerBclkGpio;
  config.gpio_cfg.ws = kSpeakerLrcGpio;
  config.gpio_cfg.dout = kSpeakerDataGpio;
  config.gpio_cfg.din = I2S_GPIO_UNUSED;
  if (i2s_channel_init_std_mode(speakerHandle, &config) == ESP_OK) return true;
  i2s_del_channel(speakerHandle);
  speakerHandle = nullptr;
  return false;
}

bool initPdmOutput() {
  i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  if (i2s_new_channel(&channel, &pdmHandle, nullptr) != ESP_OK) return false;
  i2s_pdm_tx_config_t config = {};
  config.clk_cfg = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(kSampleRate);
  config.slot_cfg = I2S_PDM_TX_SLOT_PCM_FMT_DAC_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  config.gpio_cfg.clk = I2S_GPIO_UNUSED;
  config.gpio_cfg.dout = kPdmOutGpio;
  config.gpio_cfg.dout2 = I2S_GPIO_UNUSED;
  if (i2s_channel_init_pdm_tx_mode(pdmHandle, &config) == ESP_OK) return true;
  i2s_del_channel(pdmHandle);
  pdmHandle = nullptr;
  return false;
}

void setup() {
  Serial.begin(115200);
  Serial.println("RadioRecorder setup starting");
  pinMode(kEncoderClkGpio, INPUT_PULLUP);
  pinMode(kEncoderDtGpio, INPUT_PULLUP);
  pinMode(kEncoderSwGpio, INPUT_PULLUP);
  encoderState = (digitalRead(kEncoderClkGpio) << 1) | digitalRead(kEncoderDtGpio);
  attachInterrupt(digitalPinToInterrupt(kEncoderClkGpio), onEncoderEdge, CHANGE);
  attachInterrupt(digitalPinToInterrupt(kEncoderDtGpio), onEncoderEdge, CHANGE);
  sdReady = SD_MMC.setPins(39, 38, 40) && SD_MMC.begin("/sdcard", true, false) && SD_MMC.cardType() != CARD_NONE;
  if (sdReady) recordingsPresent.store(hasRecordings());
  adcReady = initAdc();
  speakerReady = initSpeaker();
  if (speakerReady && !initPdmOutput()) Serial.println("PDM test output unavailable; speaker still works");
  if (sdReady && speakerReady && xTaskCreatePinnedToCore(speakerLoop, "speaker", 12288, nullptr, 2, &playbackTask, 0) != pdPASS) {
    Serial.println("Cannot create speaker task");
  }
  if (adcReady && xTaskCreatePinnedToCore(recordLoop, "recorder", 8192, nullptr, 2, &recorderTask, 1) != pdPASS) {
    Serial.println("Cannot create recorder task");
  }
  if (recorderTask) liveFrames = xQueueCreate(8, sizeof(AudioFrame));
  initWeb();
  printSetupStatus();
}

void loop() {
  pollEncoderRotation();
  pollSerialMeter();
  pollEncoderButton();
  if (wifiReady) {
    server.handleClient();
    audioSocket.loop();
    sendLiveFrames();
  }
  if (wifiTestPhase == 1) {
    if (captureState.load() == CaptureState::recording) {
      wifiTestSince = millis();
      wifiTestPhase = 2;
    } else if (captureState.load() == CaptureState::idle) {
      wifiTestPhase = 0;
    }
  } else if (wifiTestPhase == 2) {
    if (captureState.load() == CaptureState::idle) {
      wifiTestPhase = 0;
    } else if (millis() - wifiTestSince >= 5000) {
      if (!WiFi.mode(WIFI_OFF)) {
        Serial.println("Wi-Fi noise test: cannot disable Wi-Fi");
        stopRecording();
        wifiTestPhase = 4;
      } else {
        wifiReady = false;
        wifiTestSince = millis();
        wifiTestPhase = 3;
      }
    }
  } else if (wifiTestPhase == 3 && millis() - wifiTestSince >= 10000) {
    stopRecording();
    wifiTestPhase = 4;
  } else if (wifiTestPhase == 4 && captureState.load() == CaptureState::idle) {
    ESP.restart();
  }
  if (Serial.available()) {
    char command = Serial.read();
    if (command == 'r' && !startRecording()) Serial.println("Cannot start recording");
    if (command == 's') Serial.println(stopRecording() ? "Stop requested" : "Not recording");
    if (command == 'i') printSetupStatus();
    if (command == 'f') {
      bool enabled = !audioLowpassEnabled.load();
      audioLowpassEnabled.store(enabled);
      Serial.println(enabled ? "3.4 kHz low-pass enabled" : "3.4 kHz low-pass disabled");
    }
  }
  delay(10);
}