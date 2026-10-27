#include <Arduino.h>
#include "web_pages.h"

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
<main><h1>Felvétel</h1><p class="note" id="demoClock">DEMO 13:54:22</p><p class="state" id="state">Kapcsolódás...</p><button id="toggle" disabled>Felvétel indítása</button> <button id="wifiTest" disabled>Wi-Fi zajteszt</button><p class="note" id="message"></p>
<label class="filtercontrol"><input id="lowpassToggle" type="checkbox">3,4 kHz-es zajszűrő</label>
<p class="note">Csak ellenőrzött, kis szintű tesztforrás. A rádiót még ne csatlakoztasd.</p></main>
<section><div class="signalbar"><h2 id="signalHeading">ADC jel</h2><button id="signalToggle" disabled>Mérés bekapcsolása</button></div>
<p id="signalValues" class="signalvalues">Kikapcsolva</p><div id="signalPlot" hidden><canvas id="signalChart" aria-label="ADC RMS és csúcs idősor" role="img"></canvas>
<div class="legend"><b>RMS</b><b>Csúcs</b><span>ADC egység / 30 s</span></div></div></section>
<section><div class="signalbar"><h2>Élő hang</h2><button id="liveToggle" disabled>Hallgatás indítása</button></div>
<div class="livecontrols"><label for="liveVolume">Hangerő</label><input id="liveVolume" type="range" min="0" max="100" value="50"><button id="liveTest">Hangpróba</button></div>
<p id="liveStatus" class="note">Kikapcsolva</p></section>
<section><div class="signalbar"><h2>Mentett felvételek</h2><button id="speakerStop" disabled>ESP-lejátszás leállítása</button></div><p id="speakerStatus" class="note">ESP hangszóró: kikapcsolva</p>
<div class="speaker-volume"><label for="speakerVolume">ESP hangszóró hangereje</label><input id="speakerVolume" type="range" min="0" max="100" step="1" value="25"><div class="speaker-volume-steps"><button id="speakerDown" type="button" aria-label="Hangerő csökkentése 5%-kal" title="5%-kal halkabb">←</button><output id="speakerVolumeValue" for="speakerVolume">25%</output><button id="speakerUp" type="button" aria-label="Hangerő növelése 5%-kal" title="5%-kal hangosabb">→</button></div><label class="boost-label" for="speakerBoost">Lejátszási erősítés</label><select id="speakerBoost"><option value="1">1×</option><option value="2">2×</option><option value="4">4×</option></select></div>
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
let demoBase=13*3600+54*60+22,demoSyncedAt=performance.now();
function renderDemoClock(){
 const seconds=(demoBase+Math.floor((performance.now()-demoSyncedAt)/1000))%86400;
 document.querySelector('#demoClock').textContent='DEMO '+[Math.floor(seconds/3600),Math.floor(seconds/60)%60,seconds%60].map(value=>String(value).padStart(2,'0')).join(':');
}
setInterval(renderDemoClock,1000);
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
    demoBase=data.demoSeconds;demoSyncedAt=performance.now();renderDemoClock();
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
<main><h1>Fájlkezelő</h1><p id="demoClock">DEMO 13:54:22</p><ol id="crumbs" class="crumbs"></ol>
<div class="toolbar"><strong id="location">microSD</strong><button id="reload" title="Lista frissítése">Frissítés</button></div>
<div class="toolbar"><span id="speakerStatus">ESP hangszóró: kikapcsolva</span><button id="speakerStop" disabled>ESP-lejátszás leállítása</button></div>
<div class="speaker-volume"><label for="speakerVolume">ESP hangszóró hangereje</label><input id="speakerVolume" type="range" min="0" max="100" step="1" value="25"><div class="speaker-volume-steps"><button id="speakerDown" type="button" aria-label="Hangerő csökkentése 5%-kal" title="5%-kal halkabb">←</button><output id="speakerVolumeValue" for="speakerVolume">25%</output><button id="speakerUp" type="button" aria-label="Hangerő növelése 5%-kal" title="5%-kal hangosabb">→</button></div><label class="boost-label" for="speakerBoost">Lejátszási erősítés</label><select id="speakerBoost"><option value="1">1×</option><option value="2">2×</option><option value="4">4×</option></select></div>
<p id="message">Betöltés...</p><ol id="items" class="items"></ol><button id="more" hidden>Továbbiak</button></main>
<footer>Lejátszás a böngésző által támogatott hangformátumokkal.</footer>
<script>
const folder=new URLSearchParams(location.search).get('dir')||'/';
fetch('/api/status',{cache:'no-store'}).then(response=>response.json()).then(data=>{
 document.querySelector('#firmwareVersion').textContent='v'+data.version;
 demoBase=data.demoSeconds;demoSyncedAt=performance.now();renderDemoClock();
}).catch(()=>{});
const listEl=document.querySelector('#items'),message=document.querySelector('#message'),more=document.querySelector('#more');
const speakerStatus=document.querySelector('#speakerStatus'),speakerStop=document.querySelector('#speakerStop');
const speakerVolume=document.querySelector('#speakerVolume'),speakerVolumeValue=document.querySelector('#speakerVolumeValue'),speakerBoost=document.querySelector('#speakerBoost');
let next=0,loading=false;
let volumeQueued=null,volumeSending=false,boostUpdating=false;
let demoBase=13*3600+54*60+22,demoSyncedAt=performance.now();
function renderDemoClock(){
 const seconds=(demoBase+Math.floor((performance.now()-demoSyncedAt)/1000))%86400;
 document.querySelector('#demoClock').textContent='DEMO '+[Math.floor(seconds/3600),Math.floor(seconds/60)%60,seconds%60].map(value=>String(value).padStart(2,'0')).join(':');
}
setInterval(renderDemoClock,1000);
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
    demoBase=data.demoSeconds;demoSyncedAt=performance.now();renderDemoClock();
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

