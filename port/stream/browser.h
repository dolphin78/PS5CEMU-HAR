// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace ps5stream {
constexpr const char* kBrowser = R"HTML(<!doctype html><html><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no,viewport-fit=cover"><title>PS5CEMU-HAR Second Screen</title>
<style>html,body{margin:0;background:#111;width:100%;height:100%;overflow:hidden}img{display:block;width:100vw;height:100vh;height:100dvh;object-fit:contain;touch-action:none;user-select:none}</style>
<img id="screen" draggable="false" alt="Waiting for the second screen">
<script>
const screen=document.querySelector('#screen');
let active=null,x=0,y=0,inputSocket=null,videoSocket=null,moveScheduled=false;
const websocketURL=path=>(location.protocol==='https:'?'wss://':'ws://')+location.host+path;
function touch(down){
 if(!inputSocket||inputSocket.readyState!==1)return false;
 // Reconnect instead of accumulating old input behind a stalled connection.
 if(inputSocket.bufferedAmount>1024){inputSocket.close();return false;}
 inputSocket.send((down?1:0)+','+x+','+y);return true;
}
function connectInput(){
 if(document.hidden||inputSocket)return;
 const socket=new WebSocket(websocketURL('/input'));inputSocket=socket;
 socket.onopen=()=>{if(inputSocket!==socket)return;touch(false);};
 socket.onclose=()=>{if(inputSocket!==socket)return;inputSocket=null;active=null;if(!document.hidden)setTimeout(connectInput,1000);};
 socket.onerror=()=>socket.close();
}
function connectVideo(){
 if(document.hidden||videoSocket)return;
 const socket=new WebSocket(websocketURL('/video'));videoSocket=socket;socket.binaryType='blob';
 socket.onopen=()=>{socket.send('ready');};
 socket.onmessage=event=>{
  if(videoSocket!==socket||document.hidden)return;
  const url=URL.createObjectURL(event.data);
  const decoded=()=>{URL.revokeObjectURL(url);screen.onload=screen.onerror=null;if(videoSocket===socket&&socket.readyState===1&&!document.hidden)socket.send('ready');};
  screen.onload=screen.onerror=decoded;screen.src=url;
 };
 socket.onclose=()=>{if(videoSocket!==socket)return;videoSocket=null;if(!document.hidden)setTimeout(connectVideo,1000);};
 socket.onerror=()=>socket.close();
}
function disconnect(){release();const input=inputSocket,video=videoSocket;inputSocket=videoSocket=null;if(input)input.close();if(video)video.close();}


function position(e){const r=screen.getBoundingClientRect(),aspect=(screen.naturalWidth||854)/(screen.naturalHeight||480),w=Math.min(r.width,r.height*aspect),h=w/aspect;x=Math.max(0,Math.min(1,(e.clientX-r.left-(r.width-w)/2)/w));y=Math.max(0,Math.min(1,(e.clientY-r.top-(r.height-h)/2)/h));}
screen.onpointerdown=e=>{if(active!==null||!screen.naturalWidth||inputSocket?.readyState!==1)return;active=e.pointerId;screen.setPointerCapture(active);position(e);touch(true);e.preventDefault();};
screen.onpointermove=e=>{if(e.pointerId===active){position(e);if(!moveScheduled){moveScheduled=true;requestAnimationFrame(()=>{moveScheduled=false;if(active!==null)touch(true);});}}};
function release(){if(active!==null){active=null;touch(false);}}
screen.onpointerup=screen.onpointercancel=screen.onlostpointercapture=release;
window.addEventListener('pagehide',disconnect);document.addEventListener('visibilitychange',()=>{if(document.hidden)disconnect();else{connectInput();connectVideo();}});
setInterval(()=>{if(active!==null)touch(true);},250);
connectInput();connectVideo();
</script></html>)HTML";
}
