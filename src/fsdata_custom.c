#include "lwip/apps/fs.h"
#include "lwip/def.h"
#include <string.h>
#include "cJSON.h"

/* HTML content - headers added by lwIP (LWIP_HTTPD_DYNAMIC_HEADERS=1) */
static const char data_index_html[] = 
"<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>BL602 WiFi</title><style>"
"*{box-sizing:border-box}"
"body{font-family:-apple-system,Arial,sans-serif;max-width:400px;margin:0 auto;padding:20px;background:#f0f2f5}"
"h1{color:#1a73e8;text-align:center;margin-bottom:20px}"
".card{background:#fff;padding:20px;border-radius:12px;box-shadow:0 2px 8px rgba(0,0,0,0.1)}"
"button{background:#1a73e8;color:#fff;border:none;padding:12px;border-radius:8px;width:100%;font-size:16px;cursor:pointer}"
"button:disabled{background:#ccc}"
".btn-cancel{background:#666;margin-top:8px}"
"input{width:100%;padding:12px;margin:10px 0;border:1px solid #ddd;border-radius:8px;font-size:16px}"
"input:focus{outline:none;border-color:#1a73e8}"
".list{list-style:none;padding:0;margin:15px 0;max-height:300px;overflow-y:auto}"
".item{display:flex;align-items:center;background:#f8f9fa;padding:12px;margin:8px 0;border-radius:8px;cursor:pointer;border:2px solid transparent}"
".item:hover,.item.sel{background:#e8f0fe;border-color:#1a73e8}"
".ssid{font-weight:500;flex:1}"
".signal{display:flex;align-items:flex-end;gap:2px;height:16px}"
".bar{width:4px;background:#1a73e8;border-radius:1px}"
".bar.off{background:#ddd}"
".msg{margin-top:15px;padding:12px;border-radius:8px;text-align:center}"
".ok{background:#d4edda;color:#155724}"
".err{background:#f8d7da;color:#721c24}"
".info{background:#cce5ff;color:#004085}"
".hide{display:none}"
"#form h3{margin:0 0 10px;color:#333}"
".dash{margin-top:16px;background:#101827;color:#e5edf7;padding:16px;border-radius:12px;box-shadow:0 2px 8px rgba(0,0,0,.12)}"
".dash h2{font-size:18px;margin:0 0 12px;color:#fff}"
".metrics{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:8px}"
".metric{background:#1d293b;padding:10px;border-radius:8px;min-width:0}"
".metric small{display:block;color:#9fb0c5;font-size:11px}"
".metric strong{display:block;font-size:18px;overflow-wrap:anywhere;margin-top:4px}"
".feed{margin-top:12px;max-height:340px;overflow:auto}"
".pkt{background:#1d293b;border-radius:8px;padding:10px;margin:8px 0;font-size:12px;overflow-wrap:anywhere}"
".pkthead{color:#9fd0ff;margin-bottom:6px;font-weight:600}"
".hex{color:#a7e3c0;white-space:pre-wrap;word-break:break-word;font-family:monospace}"
".ascii{color:#fff;white-space:pre-wrap;word-break:break-word;font-family:monospace;margin-top:5px}"
".muted{color:#9fb0c5;font-size:12px;margin-top:8px}"
".toggle{width:auto;padding:7px 10px;font-size:12px;margin:0 0 8px}"
"</style></head><body>"
"<h1>WiFi Setup</h1>"
"<div class=\"card\">"
"<button id=\"scanBtn\" onclick=\"scan()\">Scan Networks</button>"
"<ul id=\"list\" class=\"list\"></ul>"
"<div id=\"form\" class=\"hide\">"
"<h3 id=\"ssidName\"></h3>"
"<input type=\"password\" id=\"pwd\" placeholder=\"Enter password\">"
"<button id=\"connBtn\" onclick=\"conn()\">Connect</button>"
"<button class=\"btn-cancel\" onclick=\"cancel()\">Cancel</button>"
"</div>"
"<div id=\"msg\" class=\"hide\"></div>"
"</div>"
"<div class=\"card\" style=\"margin-top:16px\">"
"<h3 style=\"margin-top:0\">Data Receiver</h3>"
"<label for=\"serverIp\">Server IPv4 address</label>"
"<input id=\"serverIp\" inputmode=\"decimal\" placeholder=\"e.g. 192.168.1.100\">"
"<label for=\"serverPort\">TCP port</label>"
"<input id=\"serverPort\" inputmode=\"numeric\" type=\"number\" min=\"1\" max=\"65535\" value=\"9100\">"
"<button id=\"saveServerBtn\" onclick=\"saveServer()\">Save Server Settings</button>"
"<div id=\"serverMsg\" class=\"hide\"></div>"
"<p style=\"font-size:12px;color:#666\">The receiver must be reachable on this network. Default port: 9100.</p>"
"</div>"
"<section class=\"dash\"><h2>GPMon v1.0_17102026</h2><h2>Live UART Monitor</h2>"
"<div class=\"metrics\">"
"<div class=\"metric\"><small>Packets</small><strong id=\"mPackets\">0</strong></div>"
"<div class=\"metric\"><small>RX bytes</small><strong id=\"mBytes\">0</strong></div>"
"<div class=\"metric\"><small>UART errors</small><strong id=\"mErrors\">0</strong></div>"
"<div class=\"metric\"><small>Ring overflows</small><strong id=\"mOverflow\">0</strong></div>"
"<div class=\"metric\"><small>Queue depth</small><strong id=\"mQueue\">0</strong></div>"
"<div class=\"metric\"><small>Queue high-water</small><strong id=\"mHigh\">0</strong></div>"
"<div class=\"metric\"><small>Wi-Fi</small><strong id=\"mWifi\">idle</strong></div>"
"</div><p class=\"muted\" id=\"liveStatus\">Waiting for UART data…</p>"
"<button class=\"toggle\" id=\"hexToggle\" onclick=\"toggleHex()\">Hide hex</button>"
"<div id=\"feed\" class=\"feed\"></div></section>"
"<script>"
"var ssid='',pt=null,ef=0;"
"function loadServer(){fetch('/serverget',{cache:'no-store'}).then(r=>r.json()).then(d=>{"
"if(d.ip)document.getElementById('serverIp').value=d.ip;"
"document.getElementById('serverPort').value=d.port||9100;"
"if(!d.ok&&d.error)serverMessage(d.error,'err');"
"}).catch(()=>serverMessage('Could not load saved settings','err'));}"
"function serverMessage(m,c){var e=document.getElementById('serverMsg');e.textContent=m;e.className='msg '+c;}"
"function saveServer(){var ip=document.getElementById('serverIp').value.trim();"
"var port=document.getElementById('serverPort').value.trim();"
"var b=document.getElementById('saveServerBtn');b.disabled=true;"
"fetch('/serverset?ip='+encodeURIComponent(ip)+'&port='+encodeURIComponent(port),{cache:'no-store'})"
".then(r=>r.json()).then(d=>serverMessage(d.ok?'Saved '+ip+':'+port:(d.error||'Save failed'),d.ok?'ok':'err'))"
".catch(()=>serverMessage('Save request failed','err')).finally(()=>{b.disabled=false;});}"
"loadServer();"
"function sig(r){var b='',l=r>-50?4:r>-60?3:r>-70?2:1;"
"for(var i=1;i<=4;i++)b+='<div class=\"bar'+(i<=l?'':' off')+'\" style=\"height:'+(i*4)+'px\"></div>';return b;}"
"function scan(){"
"var b=document.getElementById('scanBtn');"
"b.disabled=true;b.textContent='Scanning...';"
"document.getElementById('list').innerHTML='';"
"document.getElementById('msg').className='hide';"
"document.getElementById('form').className='hide';"
"fetchScan();"
"}"
"function fetchScan(){"
"fetch('/wifiscan').then(r=>r.text()).then(t=>{"
"var d;try{d=JSON.parse(t.trim())}catch(e){finishScanError('Scan response error');return}"
"if(d&&d.scanning){setTimeout(fetchScan,300);return}"
"if(d&&d.error){finishScanError(d.error);return}"
"var b=document.getElementById('scanBtn');"
"b.disabled=false;b.textContent='Scan Networks';"
"var l=document.getElementById('list');"
"if(!d||!d.length||d[0]==='null'){show('No networks found','err');return}"
"d.forEach(x=>{if(x.ssid){"
"var li=document.createElement('li');li.className='item';"
"li.innerHTML='<span class=\"ssid\">'+x.ssid+'</span><div class=\"signal\">'+sig(x.rssi)+'</div>';"
"li.onclick=function(){sel(this,x.ssid)};l.appendChild(li);"
"}});"
"}).catch(()=>{setTimeout(fetchScan,500);});"
"}"
"function finishScanError(msg){"
"var b=document.getElementById('scanBtn');"
"b.disabled=false;b.textContent='Scan Networks';show(msg,'err');"
"}"
"function sel(el,s){"
"document.querySelectorAll('.item').forEach(i=>i.classList.remove('sel'));"
"el.classList.add('sel');ssid=s;"
"document.getElementById('ssidName').textContent=s;"
"document.getElementById('form').className='';document.getElementById('pwd').value='';document.getElementById('pwd').focus();}"
"function conn(){"
"var p=document.getElementById('pwd').value,b=document.getElementById('connBtn');"
"b.disabled=true;b.textContent='Connecting...';show('Connecting to '+ssid+'...','info');"
"fetch('/wificonfig?ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(p))"
".then(r=>r.text()).then(t=>{"
"try{var d=JSON.parse(t.trim())}catch(e){show('Error','err');b.disabled=false;b.textContent='Connect';return}"
"if(d.state==='connecting'){poll(b);}else{show('Failed','err');b.disabled=false;b.textContent='Connect';}"
"}).catch(()=>{b.disabled=false;b.textContent='Connect';show('Request failed','err');});}"
"function poll(b){"
"var c=0;ef=0;if(pt)clearInterval(pt);"
"pt=setInterval(()=>{"
"if(++c>90){clearInterval(pt);show('Timeout','err');b.disabled=false;b.textContent='Connect';return;}"
"show('Connecting... '+c+'s','info');"
"fetch('/wifiget').then(r=>r.text()).then(t=>{"
"ef=0;try{var d=JSON.parse(t.trim())}catch(e){return}"
"if(d.state==='connected'||d.state==='ap_closed'){clearInterval(pt);"
"var m='Connected!<br>IP: '+d.ip;if(d.state==='ap_closed')m+='<br>AP Closed';else if(d.msg)m+='<br>'+d.msg;"
"show(m,'ok',1);document.getElementById('form').className='hide';b.disabled=false;b.textContent='Connect';}"
"}).catch(()=>{if(++ef>10){clearInterval(pt);show('Lost','err');b.disabled=false;b.textContent='Connect';}});},1000);}"
"function cancel(){if(pt)clearInterval(pt);document.getElementById('form').className='hide';"
"document.getElementById('connBtn').disabled=false;document.getElementById('connBtn').textContent='Connect';}"
"function show(m,c,h){var e=document.getElementById('msg');h?e.innerHTML=m:e.textContent=m;e.className='msg '+c;}"

"var showHex=true;"
"function toggleHex(){showHex=!showHex;document.getElementById('hexToggle').textContent=showHex?'Hide hex':'Show hex';"
"document.querySelectorAll('.hex').forEach(e=>e.style.display=showHex?'':'none');}"
"function metric(id,v){document.getElementById(id).textContent=String(v==null?0:v);}"
"function refreshLive(){fetch('/gpmonlive',{cache:'no-store'}).then(r=>r.json()).then(d=>{"
"metric('mPackets',d.packet_count);metric('mBytes',d.rx_bytes);metric('mErrors',d.uart_errors);"
"metric('mOverflow',d.ring_overflows);metric('mQueue',d.queue_depth);metric('mHigh',d.queue_high_water);metric('mWifi',d.wifi_state||'idle');"
"document.getElementById('liveStatus').textContent='Live · '+(d.packets?d.packets.length:0)+' recent packets';"
"var f=document.getElementById('feed');f.textContent='';"
"(d.packets||[]).forEach(p=>{var card=document.createElement('div');card.className='pkt';"
"var head=document.createElement('div');head.className='pkthead';"
"head.textContent='SEQ '+p.seq+' · '+p.len+' bytes · flags 0x'+Number(p.flags).toString(16).padStart(2,'0');card.appendChild(head);"
"var hx=document.createElement('div');hx.className='hex';hx.style.display=showHex?'':'none';hx.textContent='HEX  '+p.hex;card.appendChild(hx);"
"var asc=document.createElement('div');asc.className='ascii';asc.textContent='ASCII '+p.ascii;card.appendChild(asc);f.appendChild(card);});"
"}).catch(()=>{document.getElementById('liveStatus').textContent='Dashboard endpoint unavailable; retrying…';})"
".finally(()=>setTimeout(refreshLive,500));}"
"refreshLive();"
"</script></body></html>";

/* FS_ROOT must be defined for lwIP fs.c, set to NULL */
#define FS_ROOT NULL

void *fs_state_init(struct fs_file *file, const char *name) { return NULL; }
void fs_state_free(struct fs_file *file, void *state) {}

int fs_open_custom(struct fs_file *file, const char *name)
{
    if (!file || !name) {
        return 0;
    }

    /* Serve static index.html; never free this buffer. */
    if (name[0] == '/' &&
        (name[1] == '\0' ||
         strcmp(name, "/index.html") == 0 ||
         strcmp(name, "/index.htm") == 0)) {
        memset(file, 0, sizeof(struct fs_file));
file->data = data_index_html;
file->len = sizeof(data_index_html) - 1;
file->index = file->len;
        return 1;
    }

    /* CGI JSON buffers remain owned until fs_close_custom(). */
    if (name[0] == '{' || name[0] == '[') {
        memset(file, 0, sizeof(struct fs_file));
        file->data = name;
file->len = (int)strlen(name);
file->index = file->len;
file->pextension = (fs_file_extension *)name;
        return 1;
    }

    return 0;
}

void fs_close_custom(struct fs_file *file)
{
    if (file) {
        if (file->pextension != NULL) {
            cJSON_free(file->pextension);
            file->pextension = NULL;
        }
        file->data = NULL;
    }
}

int fs_read_custom(struct fs_file *file, char *buffer, int count)
{
    if (!file || !file->data) return -1;
    int remain = file->len - file->index;
    if (remain <= 0) return -1;
    int n = (count < remain) ? count : remain;
    memcpy(buffer, file->data + file->index, n);
    file->index += n;
    return n;
}
