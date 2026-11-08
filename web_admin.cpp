#include "web_admin.h"
#include "app_config.h"
#include "radio_player.h"
#include "stock_feed.h"
#include "ui_display.h"
#include "wifi_mgr.h"
#include "gyro_imu.h"
#include "buttons.h"
#include "beep.h"
#include "cfg_pins.h"
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <esp_random.h>
#include <Update.h>
#include <time.h>

static AsyncWebServer server(80);

static String s_token = "";
static uint32_t s_powerOffAt = 0;
static volatile bool s_otaDone = false;   // OTA 收完 -> 回响应后再重启

// v7.1 严格 I2C 姿态传感器探测: 同 WiFi 扫描, 放主循环跑, 结果轮询读取
static volatile bool s_imuReq  = false;
static volatile bool s_imuDone = false;
static volatile int  s_imuPwr  = -1;
static String s_imuOut = "";
static int    s_imuN   = 0;

// WiFi 扫描: 在 loop 里做(不阻塞 async_tcp 任务), 页面轮询结果
static volatile bool s_scanReq  = false;
static volatile bool s_scanDone = false;
static String s_scanJson = "[]";
static String s_scanErr  = "";
static SemaphoreHandle_t s_scanMux = nullptr;   // 保护上面两个 String 的跨任务访问

static bool isAuthed(AsyncWebServerRequest* r) {
  if (!s_token.length()) return false;
  if (r->hasHeader("Cookie")) {
    const AsyncWebHeader* h = r->getHeader("Cookie");
    if (h && h->value().indexOf("box0tok=" + s_token) >= 0) return true;
  }
  return false;
}

// 救急通道鉴权: 允许用管理密码代替 cookie (手机地址栏直接敲 URL 用)
static bool isAuthedOrPw(AsyncWebServerRequest* r) {
  if (isAuthed(r)) return true;
  if (r->hasParam("pw")) {
    String p = r->getParam("pw")->value();
    if (p == g_adminPass) return true;
  }
  return false;
}

// ---------------- 请求体读取 ----------------
// ESPAsyncWebServer 3.x 把原始 body 的参数名从 "plain" 改成了 "body",
// 且 application/json 根本不生成参数。因此约定: 数据一律放在
//   POST /api/xxx?d=<urlencode(json)>
// 由库的 query 解析(稳定且自动 urlDecode)。body 仅作兜底。
static void collectBody(AsyncWebServerRequest* r, uint8_t* data, size_t len,
                        size_t index, size_t total) {
  if (index == 0) {
    if (r->_tempObject) { delete static_cast<String*>(r->_tempObject); r->_tempObject = nullptr; }
    r->_tempObject = new String();
    if (total > 4096) total = 4096;
    static_cast<String*>(r->_tempObject)->reserve(total + 16);
  }
  String* s = static_cast<String*>(r->_tempObject);
  if (s && s->length() + len <= 4096) s->concat(reinterpret_cast<const char*>(data), len);
}

static String takeBody(AsyncWebServerRequest* r) {
  String b;
  if (r->_tempObject) {
    b = *static_cast<String*>(r->_tempObject);
    delete static_cast<String*>(r->_tempObject);
    r->_tempObject = nullptr;
  }
  if (!b.length() && r->hasParam("body", true))  b = r->getParam("body", true)->value();
  if (!b.length() && r->hasParam("plain", true)) b = r->getParam("plain", true)->value();
  return b;
}

static String qs(AsyncWebServerRequest* r, const char* name) {
  if (!r->hasParam(name)) return String();
  return r->getParam(name)->value();
}

// ---------------- 页面 ----------------
static const char PAGE_LOGIN[] PROGMEM = R"rawlite(
<!DOCTYPE html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>BOX0 登录</title>
<style>
body{font-family:-apple-system,system-ui,sans-serif;background:#111;color:#eee;
display:flex;justify-content:center;padding-top:70px;margin:0}
.c{background:#1c1c1c;padding:26px;border-radius:14px;width:280px}
input,button{width:100%;padding:12px;margin:8px 0;border-radius:10px;
border:1px solid #444;box-sizing:border-box;font-size:16px}
input{background:#111;color:#eee}
button{background:#1d9e75;color:#fff;border:none;font-size:16px}
h3{text-align:center;margin:0 0 6px}
.t{text-align:center;color:#888;font-size:13px;line-height:1.6}
</style></head><body><div class=c>
<h3>BOX0 配置</h3>
<form method=POST action=/login>
<input name=pass type=password placeholder="管理密码" autofocus>
<button>登录</button></form>
<div class=t>初始密码 123456<br>忘了密码只能长按 M 键选"恢复出厂"</div>
</div></body></html>
)rawlite";

static const char PAGE_MAIN[] PROGMEM = R"rawlite(
<!DOCTYPE html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1,maximum-scale=1">
<title>BOX0</title>
<style>
*{box-sizing:border-box}
body{font-family:-apple-system,system-ui,sans-serif;background:#0e0e0e;color:#eee;
margin:0;padding:10px 10px 40px;max-width:560px;margin:0 auto}
h3{margin:14px 0 6px;font-size:15px;color:#ddd}
.card{background:#1b1b1b;border-radius:12px;padding:10px 12px;margin:8px 0;font-size:13px;line-height:1.6}
nav{display:flex;gap:6px;margin:10px 0}
nav button{flex:1;padding:11px 0;background:#1b1b1b;color:#999;border:none;
border-radius:10px;font-size:14px}
nav button.on{background:#1d9e75;color:#fff}
.tab{display:none}.tab.on{display:block}
textarea,input,select{width:100%;padding:10px;margin:5px 0;background:#1a1a1a;color:#eee;
border:1px solid #333;border-radius:10px;font-size:15px;font-family:inherit}
textarea{font-family:ui-monospace,Menlo,monospace;font-size:13px;line-height:1.5}
button.act{background:#1d9e75;color:#fff;border:none;border-radius:10px;
padding:12px;font-size:15px;width:100%;margin:8px 0}
button.gray{background:#2a2a2a;color:#ccc}
button.red{background:#c0392b}
.hint{color:#888;font-size:12px;line-height:1.6;margin:6px 0}
.warn{color:#e67e22;font-size:12px}
label{font-size:13px;color:#aaa;display:block;margin-top:10px}
.row{display:flex;gap:8px}.row>*{flex:1}
.chip{display:inline-block;background:#242424;color:#bbb;border-radius:14px;
padding:6px 12px;margin:4px 4px 0 0;font-size:12px}
.ap{background:#1a1a1a;border:1px solid #2d2d2d;border-radius:10px;padding:11px 12px;
margin:5px 0;font-size:14px;display:flex;justify-content:space-between}
.ap span{color:#777;font-size:12px}
#msg{position:fixed;left:50%;bottom:20px;transform:translateX(-50%);background:#1d9e75;
color:#fff;padding:10px 20px;border-radius:22px;display:none;font-size:14px;
box-shadow:0 4px 16px rgba(0,0,0,.5);max-width:90%;text-align:center}
#info{white-space:pre-line;color:#999;font-size:12px}
</style></head><body>

<div class=card>
  <div style="display:flex;justify-content:space-between">
    <b>BOX0 收音机 · 行情</b><span id=ver style="color:#666"></span>
  </div>
  <div id=netline style="color:#bbb">正在读取状态...</div>
</div>

<nav>
<button class=on onclick="show(0,this)">股票</button>
<button onclick="show(1,this)">设置</button>
<button onclick="show(2,this)">设备</button>
<button onclick="show(3,this)">升级</button>
</nav>

<div class="tab on" id=t0>
  <h3>自选股（每行一个，最多 30 只）</h3>
  <textarea id=codes rows=8 spellcheck=false placeholder="600519 或 sh600519"></textarea>
  <div class=hint>A股可直接填 6 位数字：6开头=沪、0/3开头=深、8/4开头=北交所<br>
  指数 sh000001、深成指 sz399001　港股 rt_hk+5位　美股 gb_+代码（小写）</div>
  <div id=quick></div>
  <button class=act onclick=saveStocks()>保存自选股</button>
</div>

<div class="tab" id=t1>
  <h3>显示</h3>
  <label>股票显示方式</label>
  <select id=disp onchange="post('/api/settings',{disp:+this.value})">
    <option value=0>单只大屏（大字现价 + 涨跌 + 开高低额）</option>
    <option value=1>列表模式（一屏 6 只，每几秒自动翻页）</option>
    <option value=2>定时自动（列表/单只自动交替）</option>
  </select>
  <div class=hint>改动立即生效，盒子屏幕会刷新。<br>
  列表模式：左右短按手动翻页，到"轮播间隔"自动翻页；左右长按仍是音量。<br>
  <b>本板未焊六轴传感器，重力翻转硬件上做不到</b>；"定时自动"是可用替代，按下面间隔在列表与单只大屏之间自动切换。若日后探测到 IMU 会自动改为重力优先。</div>
  <label>自动切换间隔（仅"定时自动"模式）</label>
  <select id=aswsec onchange="post('/api/settings',{aswsec:+this.value})">
    <option value=0>不自动切换</option>
    <option value=10>10 秒</option>
    <option value=20>20 秒</option>
    <option value=30>30 秒</option>
    <option value=60>1 分钟</option>
    <option value=120>2 分钟</option>
  </select>
  <label>屏幕亮度 <span id=brtv style="color:#1d9e75"></span></label>
  <input type=range id=brt min=10 max=100 step=5
    oninput="brtv.innerText=this.value+'%'"
    onchange="post('/api/settings',{bright:+this.value})">
  <label>音量 <span id=volv style="color:#1d9e75"></span></label>
  <input type=range id=vol min=0 max=100 step=5
    oninput="volv.innerText=this.value"
    onchange="post('/api/settings',{volume:+this.value})">

  <h3>时间范围</h3>
  <div class=card>两套时段<b>互相独立</b>：「信息更新」决定何时抓行情，「预警提示」决定何时允许响铃。<br>
  默认 9:00–11:30 / 13:00–15:00。收盘后不再刷新、也不再提醒，不会在 15:00 之后突然"滴滴"。</div>
  <label>信息更新时段（行情抓取）</label>
  <div class=row>
    <div><label>上午 起</label><input id=famS type=time></div>
    <div><label>上午 止</label><input id=famE type=time></div>
  </div>
  <div class=row>
    <div><label>下午 起</label><input id=fpmS type=time></div>
    <div><label>下午 止</label><input id=fpmE type=time></div>
  </div>
  <label>预警提示时段（涨跌响铃）</label>
  <div class=row>
    <div><label>上午 起</label><input id=aamS type=time></div>
    <div><label>上午 止</label><input id=aamE type=time></div>
  </div>
  <div class=row>
    <div><label>下午 起</label><input id=apmS type=time></div>
    <div><label>下午 止</label><input id=apmE type=time></div>
  </div>
  <div class=row>
    <div><label>仅工作日生效</label><select id=twkd>
      <option value=1>是（周一~周五）</option><option value=0>否（每天）</option></select></div>
    <div><label>时段外仍抓行情</label><select id=ftout>
      <option value=0>否（不抓，省电）</option><option value=1>是（每 5 分钟）</option></select></div>
  </div>

  <h3>行情</h3>
  <div class=row>
    <div><label>刷新间隔（秒）</label><input id=ref type=number min=3 max=120></div>
    <div><label>轮播间隔（秒）</label><input id=jmp type=number min=2 max=60></div>
  </div>
  <label>涨跌幅提醒</label>
  <div class=row>
    <div>
      <select id=alton>
        <option value=1>开启提醒</option>
        <option value=0>关闭提醒</option>
      </select>
    </div>
    <div><label>触发阈值（%）</label><input id=altpct type=number min=1 max=20 step=0.5></div>
  </div>
  <div class=hint>只在上面的「预警提示」时段内检查。全局阈值对"留空"的股票生效；下面可给单只股票单独设阈值，<b>改某只不会影响其它股票的触发记录</b>。每只每天最多提醒一次：弹屏 12 秒 + 两声"滴滴"，<b>没有语音播报</b>。</div>
  <label>每只股票独立阈值（留空=跟随全局，-1=这只不提醒）</label>
  <div id=perbox class=card>读取中...</div>
  <label>无操作自动熄屏（分钟）</label>
  <select id=aoff onchange="post('/api/settings',{autooff:+this.value})">
    <option value=0>不自动熄屏</option>
    <option value=1>1 分钟</option>
    <option value=3>3 分钟</option>
    <option value=5>5 分钟</option>
    <option value=10>10 分钟</option>
    <option value=30>30 分钟</option>
    <option value=60>60 分钟</option>
    <option value=120>120 分钟</option>
  </select>
  <label>自动关机（息屏后多久）</label>
  <select id=poff onchange="post('/api/settings',{poweroffmin:+this.value})">
    <option value=0>关闭（只熄屏，不关机）</option>
    <option value=30>息屏后 30 分钟</option>
    <option value=60>息屏后 1 小时</option>
    <option value=120>息屏后 2 小时</option>
    <option value=240>息屏后 4 小时</option>
  </select>
  <div class=hint>熄屏后按任意键点亮（第一次按键只唤醒，不会误操作）。<br>
  <b>v7.1 已修</b>：旧版把常态为低电平的 M 键放进深睡唤醒掩码，刚进深睡就被唤醒，看起来像"没关机"；现改为只用左/右键唤醒，电池供电仍按 M 键开机。计时也不再被弹屏/toast 清零。</div>

  <h3>安全</h3>
  <label>新的管理密码（4-15 位，留空不改）</label>
  <input id=apass type=password autocomplete=new-password>
  <button class=act onclick=saveSet()>保存设置（含时间范围与每股阈值）</button>
</div>

<div class="tab" id=t2>
  <h3>WiFi</h3>
  <div class=card id=wstat>读取中...</div>
  <button class="act gray" onclick=doScan()>扫描附近 WiFi</button>
  <div id=scanbox></div>
  <div class=row>
    <div><label>WiFi 名称</label><input id=ssid autocapitalize=off></div>
    <div><label>密码</label><input id=wpass type=password></div>
  </div>
  <button class=act onclick=saveWifi()>保存并连接</button>
  <div class=hint>① <b>密码留空即可连接无密码开放网络</b>。<br>
  ② 热点 <b>BOX0-Setup</b> 永远开启：连不上时手机连它即可重新配置。<br>
  ③ 只支持 2.4GHz；5GHz 名称通常带 -5G，请选另一个。</div>

  <h3>已保存的 WiFi（最多 8 组）</h3>
  <div class=card id=wplist>读取中...</div>
  <div class=hint>保存过的网络会记住密码。换地点（家/公司）时设备自动扫描并连接信号最好的已存网络，<b>不用重新配网</b>；这里可一键切换或删除。</div>

  <h3>设备</h3>
  <div class=card><div id=info></div></div>
  <button class="act gray" onclick=showDiag()>查看诊断 JSON</button>
  <button class="act gray" onclick=doImu()>严格探测姿态传感器（I2C 全总线）</button>
  <button class="act gray" onclick=doRefresh()>立即刷新行情</button>
  <button class="act gray" onclick=doReconnect()>重新连接 WiFi</button>
  <button class="act gray" onclick=testBeep()>测试提醒音（盒子喇叭）</button>
  <button class="act red" onclick=doReset()>恢复出厂设置</button>
  <button class="act red" onclick=doPowerOff()>关机</button>
  <button class="act gray" onclick="location.href='/logout'">退出登录</button>
  <pre id=diagbox style="display:none;background:#111;border:1px solid #333;border-radius:8px;
    padding:10px;font-size:11px;white-space:pre-wrap;word-break:break-all;color:#9c9"></pre>
  <div class=hint>关机后需按一下中键 M 才能重新开机。恢复出厂会清除 WiFi、自选股和密码。<br>
  扫描 WiFi 在已连接状态下也可用，选中新网络后点"保存并连接"即可换网。</div>
</div>

<div class="tab" id=t3>
  <h3>固件升级（网页 OTA）</h3>
  <div class=card>当前固件 <b id=fwver>-</b><br>
  选择编译好的 <code>.bin</code> 应用镜像上传，约 8 秒写完并自动重启。<br>
  <b>全程不用插 USB、不用进下载模式、不用按 RESET。</b></div>
  <input type=file id=fwfile accept=".bin">
  <button class=act onclick=uploadFw()>上传并升级</button>
  <div id=fwprog class=hint></div>
  <div class=hint>也可用独立升级页 <a href=/ota style="color:#1d9e75">/ota</a>。<br>
  升级过程中请勿断电；重启后约 5 秒恢复，本页面会自动重连。</div>

  <h3>全部接口（点击即调用）</h3>
  <div class=hint>固件提供的接口都在这里。只读/无副作用的接口可直接点，原始返回显示在下方；<br>
  写配置类接口（股票/设置/WiFi/关机/重置）已做成上方各页的表单按钮。</div>
  <div id=apilist></div>
  <pre id=apires style="background:#111;border:1px solid #333;border-radius:8px;padding:10px;
    font-size:11px;white-space:pre-wrap;word-break:break-all;color:#9c9;min-height:40px">（点上面的接口按钮查看返回）</pre>
  <div class=hint>写操作入口：股票 <b>/api/stocks</b>、设置 <b>/api/settings</b>、
  配网 <b>/api/wifi</b>、提示音 <b>/api/beep</b>、关机 <b>/api/power</b>、
  恢复出厂 <b>/api/reset</b>、固件 <b>/api/ota</b>。</div>
</div>

<div id=msg></div>
<script>
var CFG={},SCAN=[];
function show(i,b){
  for(var k=0;k<4;k++)document.getElementById('t'+k).className='tab'+(k==i?' on':'');
  var bs=document.querySelectorAll('nav button');
  for(var k=0;k<bs.length;k++)bs[k].className=(k==i?'on':'');
}
function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')}
function msg(t,ok){
  var m=document.getElementById('msg');
  m.innerText=t;m.style.display='block';
  m.style.background=(ok===false)?'#c0392b':'#1d9e75';
  clearTimeout(m._t);m._t=setTimeout(function(){m.style.display='none'},2200);
}
async function post(url,obj){
  var s=JSON.stringify(obj||{});
  var d=encodeURIComponent(s);
  var opt={method:'POST',cache:'no-store'};
  var u=url;
  // 短数据走查询串(库解析稳定); 长数据(电台列表)走 body, 避免 URL 过长
  if(d.length<=1000){u=url+'?d='+d}
  else{opt.headers={'Content-Type':'application/json'};opt.body=s}
  var r;
  try{r=await fetch(u,opt)}catch(e){msg('网络错误',false);return{}}
  if(r.status==401){location.href='/';return{}}
  var j={};try{j=await r.json()}catch(e){}
  if(j.ok)msg('已保存');else msg(j.err||('操作失败 '+r.status),false);
  return j;
}
function paint(d){
  if(d.version)document.getElementById('ver').innerText=d.version;
  var s='<b>'+d.wifiStateText+'</b>';
  if(d.wifiState==2)s+=' · IP '+d.wifiIp+' · 信号 '+d.rssi+'dBm';
  else s+=' · 热点 192.168.4.1';
  var w=document.getElementById('netline');if(w)w.innerHTML=s;
  var e=document.getElementById('wstat');
  if(e){
    var t='<b>'+d.wifiStateText+'</b>';
    if(d.wifiState==2)t+='<br>IP '+d.wifiIp+' · 信号 '+d.rssi+' dBm';
    else t+='<br>热点地址 http://192.168.4.1';
    if(d.wifiRcText)t+='<div class=warn>上次失败：'+d.wifiRcText+'</div>';
    e.innerHTML=t;
  }
  var i=document.getElementById('info');if(i)i.innerText=d.info;
}
async function load(){
  var r=await fetch('/api/cfg',{cache:'no-store'});
  if(r.status!=200){location.href='/';return}
  CFG=await r.json();
  document.getElementById('codes').value=CFG.stocks.join('\n');
  // 电台控件已移除: 不能再访问不存在的 stas/cur 元素,
  // 否则 load() 在这里抛 TypeError, 后面亮度/音量/提醒/熄屏等设置项全部加载不到。
  document.getElementById('brt').value=CFG.bright;
  document.getElementById('brtv').innerText=CFG.bright+'%';
  document.getElementById('vol').value=CFG.volume;
  document.getElementById('volv').innerText=CFG.volume;
  document.getElementById('ref').value=CFG.refresh;
  document.getElementById('jmp').value=CFG.jump;
  document.getElementById('aoff').value=CFG.autooff;
  document.getElementById('disp').value=CFG.disp;
  document.getElementById('aswsec').value=CFG.aswsec;
  document.getElementById('alton').value=CFG.alton;
  document.getElementById('altpct').value=CFG.altpct;
  document.getElementById('poff').value=CFG.poweroffmin;
  document.getElementById('ssid').value=CFG.ssid;
  document.getElementById('twkd').value=CFG.twkd;
  document.getElementById('ftout').value=CFG.ftout;
  putWin('f',CFG.fetchwin); putWin('a',CFG.alertwin);
  var fv=document.getElementById('fwver');if(fv)fv.innerText=CFG.version||'-';
  renderWifiList();
  renderPer();
  paint(CFG);
  buildQuick();
}
// ---- v7.1 时间范围: HH:MM <-> 当日分钟数 ----
function m2t(m){m=+m||0;return ('0'+Math.floor(m/60)).slice(-2)+':'+('0'+(m%60)).slice(-2)}
function t2m(s){if(!s)return 0;var p=s.split(':');return (+p[0])*60+(+p[1])}
function putWin(p,w){
  w=w||[540,690,780,900];
  document.getElementById(p+'amS').value=m2t(w[0]);
  document.getElementById(p+'amE').value=m2t(w[1]);
  document.getElementById(p+'pmS').value=m2t(w[2]);
  document.getElementById(p+'pmE').value=m2t(w[3]);
}
function getWin(p){
  return [t2m(document.getElementById(p+'amS').value),
          t2m(document.getElementById(p+'amE').value),
          t2m(document.getElementById(p+'pmS').value),
          t2m(document.getElementById(p+'pmE').value)];
}
// ---- v7.1 每只股票独立提醒阈值 ----
function renderPer(){
  var box=document.getElementById('perbox');if(!box)return;
  var st=CFG.stocks||[],per=CFG.altper||[];
  var h='<div class=hint>共 '+st.length+' 只 · 全局阈值 '+CFG.altpct+'% · 留空跟随全局，填 -1 关闭该股提醒</div>';
  for(var i=0;i<st.length;i++){
    var v=(per[i]===undefined||per[i]===0)?'':per[i];
    h+='<div class=row style="margin:2px 0"><div style="flex:1"><label>'+esc(st[i])+'</label></div>'+
       '<div style="width:96px"><input data-per="'+i+'" type=number min=-1 max=20 step=0.5 value="'+v+'" placeholder="全局"></div></div>';
  }
  box.innerHTML=h;
}
async function doImu(){
  var box=document.getElementById('diagbox');
  box.style.display='block';
  box.textContent='正在严格扫描 I2C 总线 0x08-0x77（两次 ACK 确认）…';
  try{
    await fetch('/api/imu',{cache:'no-store'});
    for(var i=0;i<25;i++){
      await new Promise(function(res){setTimeout(res,400)});
      var r=await fetch('/api/imu?get=1',{cache:'no-store'}),j=await r.json();
      if(j.done){box.textContent=JSON.stringify(j,null,2);return}
      box.textContent='扫描中… '+(i+1)+'/25';
    }
    box.textContent='扫描超时，请稍后访问 /api/imu?get=1 查看结果';
  }catch(e){box.textContent='扫描失败: '+e}
}
var QK=[['上证指数','sh000001'],['深证成指','sz399001'],['创业板指','sz399006'],
['贵州茅台','sh600519'],['宁德时代','sz300750'],['比亚迪','sz002594'],
['腾讯控股','rt_hk00700'],['阿里巴巴','rt_hk09988'],['苹果','gb_aapl'],['英伟达','gb_nvda']];
function buildQuick(){
  var h='<div class=hint>快速添加：</div>';
  QK.forEach(function(q){h+='<span class=chip onclick="addCode(\''+q[1]+'\')">'+q[0]+'</span>'});
  document.getElementById('quick').innerHTML=h;
}
function addCode(c){
  var t=document.getElementById('codes');
  var arr=t.value.split('\n').map(function(x){return x.trim()}).filter(function(x){return x});
  if(arr.indexOf(c)>=0){msg('已在列表中',false);return}
  if(arr.length>=30){msg('最多 30 只，请先删掉一只',false);return}
  arr.push(c);t.value=arr.join('\n');
}
async function saveStocks(){
  var v=document.getElementById('codes').value.split('\n')
    .map(function(s){return s.trim().toLowerCase()}).filter(function(s){return s});
  if(!v.length){msg('至少填一个代码',false);return}
  var d=await post('/api/stocks',{codes:v});
  if(d.ok){document.getElementById('codes').value=v.join('\n');setTimeout(poll,1200)}
}
function saveSet(){
  var o={refresh:+document.getElementById('ref').value,
         jump:+document.getElementById('jmp').value,
         autooff:+document.getElementById('aoff').value,
         poweroffmin:+document.getElementById('poff').value,
         aswsec:+document.getElementById('aswsec').value,
         alton:+document.getElementById('alton').value,
         altpct:+document.getElementById('altpct').value,
         twkd:+document.getElementById('twkd').value,
         ftout:+document.getElementById('ftout').value,
         fetchwin:getWin('f'),
         alertwin:getWin('a')};
  var per=[],els=document.querySelectorAll('[data-per]');
  for(var i=0;i<els.length;i++){
    var s=(els[i].value||'').trim();
    per.push(s===''?0:parseFloat(s));
  }
  o.altper=per;
  var p=document.getElementById('apass').value;
  if(p)o.adminpass=p;
  post('/api/settings',o).then(function(d){
    if(d&&d.ok){document.getElementById('apass').value='';msg('设置已保存并生效')}
  });
}
var scanTimer=null;
async function doScan(){
  document.getElementById('scanbox').innerHTML='<div class=hint>正在扫描，约 3 秒…</div>';
  try{await fetch('/api/scan',{cache:'no-store'})}catch(e){}
  var n=0;
  clearInterval(scanTimer);
  scanTimer=setInterval(async function(){
    n++;
    var j;
    try{var r=await fetch('/api/scanres',{cache:'no-store'});j=await r.json()}
    catch(e){return}
    if(!j.done){if(n>25)clearInterval(scanTimer);return}
    clearInterval(scanTimer);
    if(j.err){document.getElementById('scanbox').innerHTML='<div class=hint>'+esc(j.err)+'</div>';return}
    SCAN=j.list||[];
    if(!SCAN.length){document.getElementById('scanbox').innerHTML='<div class=hint>没扫到网络，请手动填写名称</div>';return}
    var h='';
    for(var i=0;i<SCAN.length;i++){
      var lock=SCAN[i].e==0?' 开放(无密码)':' 加密';
      h+='<div class=ap onclick="pick('+i+')">'+esc(SCAN[i].s)+lock+
         '<span>'+SCAN[i].r+'dBm</span></div>';
    }
    document.getElementById('scanbox').innerHTML=h;
  },600);
}
function pick(i){
  document.getElementById('ssid').value=SCAN[i].s;
  var open=SCAN[i].e==0;
  if(open)document.getElementById('wpass').value='';  // 开放网络: 清空密码
  msg(open?'已选开放网络(无需密码)：'+SCAN[i].s:'已选：'+SCAN[i].s);
  document.getElementById('wpass').focus();
}
async function saveWifi(){
  var s=document.getElementById('ssid').value.trim();
  if(!s){msg('请填写或选择 WiFi 名称',false);return}
  var d=await post('/api/wifi',{ssid:s,pass:document.getElementById('wpass').value});
  if(d.ok){setTimeout(poll,1500);setTimeout(poll,5000);setTimeout(poll,10000)}
}
function doPowerOff(){
  if(confirm('确定关机？关机后需按一下中键 M 才能开机。'))post('/api/power',{cmd:'off'});
}
function testBeep(){
  post('/api/beep',{}).then(function(d){if(d&&d.ok)msg('已触发提醒音')});
}
function doReset(){
  if(!confirm('恢复出厂会清除 WiFi、自选股、电台和密码，确定吗？'))return;
  post('/api/reset',{}).then(function(d){
    if(d&&d.ok){msg('已恢复出厂，请重新配置 WiFi');setTimeout(function(){location.reload()},1600)}
  });
}
async function poll(){
  try{
    var r=await fetch('/api/cfg',{cache:'no-store'});
    if(r.status!=200)return;
    paint(await r.json());
  }catch(e){}
}
// ---------- v7.0: 历史 WiFi / 接口总览 / 网页 OTA ----------
function renderWifiList(){
  var box=document.getElementById('wplist');if(!box)return;
  var list=CFG.wifiprofiles||[];
  if(!list.length){box.innerHTML='<span style="color:#888">还没有保存过 WiFi。在上面填名称（密码可留空）点「保存并连接」即可记住。</span>';return}
  var h='';
  for(var i=0;i<list.length;i++){
    var cur=CFG.ssid&&list[i].s==CFG.ssid;
    h+='<div class=ap style="align-items:center"><span>'+(cur?'★ ':'')+esc(list[i].s)+
       (list[i].o?' <span style="color:#888">(开放)</span>':'')+'</span><span style="white-space:nowrap">'+
       '<button class=act style="width:auto;padding:5px 10px;margin:0 4px;font-size:12px" onclick="wifiUse('+i+')">连接</button>'+
       '<button class="act gray" style="width:auto;padding:5px 10px;margin:0;font-size:12px" onclick="wifiDel('+i+')">删除</button>'+
       '</span></div>';
  }
  box.innerHTML=h;
}
async function wifiUse(i){
  var d=await post('/api/wifisel',{idx:i});
  if(d&&d.ok){msg('正在切换，稍后自动生效');setTimeout(load,2500)}
}
async function wifiDel(i){
  if(!confirm('删除这条已保存的 WiFi？'))return;
  var d=await post('/api/wifidel',{idx:i});
  if(d&&d.ok)setTimeout(load,400);
}
function doRefresh(){post('/api/refresh',{}).then(function(d){if(d&&d.ok)msg('已请求刷新行情')})}
function doReconnect(){post('/api/reconnect',{}).then(function(d){if(d&&d.ok)msg('正在重新连接')})}
async function showDiag(){
  var p=document.getElementById('diagbox');
  p.style.display='block';p.innerText='读取中...';
  try{var r=await fetch('/api/diag',{cache:'no-store'});p.innerText=JSON.stringify(await r.json(),null,2)}
  catch(e){p.innerText='读取失败：'+e}
}
async function uploadFw(){
  var f=document.getElementById('fwfile').files[0],p=document.getElementById('fwprog');
  if(!f){msg('请先选择 .bin 固件',false);return}
  if(!confirm('确定把 '+f.name+'（'+(f.size/1024/1024).toFixed(2)+' MB）刷入设备？'))return;
  p.innerText='正在上传 '+f.name+' ... 请勿断电';
  var fd=new FormData();fd.append('f',f);
  try{
    var r=await fetch('/api/ota',{method:'POST',body:fd}),t=await r.text();
    if(r.status==200){p.innerText='升级成功：'+t+'，设备正在重启...';msg('固件已写入，等待重启');
      setTimeout(function(){location.reload()},12000)}
    else{p.innerText='升级失败 HTTP '+r.status+'：'+t;msg('升级失败',false)}
  }catch(e){p.innerText='连接中断（设备可能已开始重启）：'+e;msg('连接中断',false)}
}
// 只读/无副作用接口: 点击直接调用并展示原始返回
var APIS=[
 ['GET','/api/cfg','读取全部配置'],
 ['GET','/api/diag','设备诊断'],
 ['GET','/api/wifi','网络状态'],
 ['GET','/api/scanres','上次扫描结果'],
 ['GET','/api/scan','触发 WiFi 扫描'],
 ['GET','/api/imu','触发严格 I2C 探测'],
 ['GET','/api/imu?get=1','读取 I2C 探测结果'],
 ['POST','/api/beep','测试提示音'],
 ['POST','/api/refresh','立即刷新行情'],
 ['POST','/api/reconnect','重新连接 WiFi']
];
function renderApis(){
  var h='';
  for(var i=0;i<APIS.length;i++){
    var a=APIS[i];
    h+='<button class="act gray" style="font-size:13px;padding:9px;margin:4px 0" onclick="callApi('+i+')">'+
       a[0]+' '+esc(a[1])+' — '+esc(a[2])+'</button>';
  }
  document.getElementById('apilist').innerHTML=h;
}
async function callApi(i){
  var a=APIS[i],out=document.getElementById('apires');
  out.innerText='请求中...';
  try{
    var opt={method:a[0],cache:'no-store'},u=a[1];
    if(a[0]=='POST'){u=a[1]+'?d='+encodeURIComponent('{}');opt.body=''}
    var r=await fetch(u,opt),t=await r.text(),s=t;
    try{s=JSON.stringify(JSON.parse(t),null,2)}catch(e){}
    out.innerText=a[0]+' '+a[1]+'  ->  HTTP '+r.status+'\n'+s;
  }catch(e){out.innerText='请求失败：'+e}
}
renderApis();
load();
setInterval(poll,4000);
</script></body></html>
)rawlite";

// ---------------- 工具 ----------------
// 纯 6 位数字 -> 自动补市场前缀:
//   首位 6 -> 沪 sh; 首位 0/3 -> 深 sz; 首位 8/4 -> 北交所 bj; 9 -> bj
//   已带 sh/sz/bj/rt_hk/gb_ 前缀的保持不变
static String normalizeCode(const String& c) {
  if (c.length() == 6) {
    bool allDigit = true;
    for (int i = 0; i < 6; i++) if (!isDigit(c[i])) { allDigit = false; break; }
    if (allDigit) {
      char f = c[0];
      if (f == '6') return "sh" + c;                       // 沪市 A 股/指数
      if (f == '0' || f == '3') return "sz" + c;           // 深市 A 股/指数
      if (f == '5') return "sh" + c;                       // 沪市 ETF/基金(51xxxx/56xxxx)
      if (f == '1') return "sz" + c;                       // 深市 ETF/基金(15xxxx/16xxxx)
      if (f == '8' || f == '4' || f == '9') return "bj" + c; // 北交所
      return c;   // 其它开头(2 等)无法判断, 保持原样交给 validCode 报错
    }
  }
  return c;
}

static bool validCode(const String& c) {
  if (c.length() < 5 || c.length() > 12) return false;
  if ((c.startsWith("sh") || c.startsWith("sz") || c.startsWith("bj")) && c.length() == 8) {
    for (int i = 2; i < 8; i++) if (!isDigit(c[i])) return false;
    return true;
  }
  if (c.startsWith("rt_hk") && c.length() == 10) {
    for (int i = 5; i < 10; i++) if (!isDigit(c[i])) return false;
    return true;
  }
  if (c.startsWith("gb_") && c.length() >= 6) {
    for (int i = 3; i < (int)c.length(); i++) {
      char ch = c[i];
      if (!isalnum(ch) && ch != '.') return false;
    }
    return true;
  }
  return false;
}

static void sendOk(AsyncWebServerRequest* r, bool ok, const char* err = "") {
  AsyncJsonResponse* resp = new AsyncJsonResponse();
  JsonVariant j = resp->getRoot();
  j["ok"] = ok;
  if (err && *err) j["err"] = err;
  resp->setLength();
  r->send(resp);
}

static String jsonEsc(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n' || c == '\r' || c == '\t') o += ' ';
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o;
}

// 鉴权 + 取数据 + 分发
template <typename F>
static ArRequestHandlerFunction _post(F fn) {
  return [fn](AsyncWebServerRequest* r) {
    if (!isAuthed(r)) { r->send(401); return; }
    String b = qs(r, "d");            // 主通道: query
    if (!b.length()) b = takeBody(r); // 兜底: body
    if (!b.length()) { sendOk(r, false, "空请求"); return; }
    fn(r, b);
  };
}

static void handleCfg(AsyncWebServerRequest* r) {
  AsyncJsonResponse* resp = new AsyncJsonResponse(false);
  JsonVariant j = resp->getRoot();
  JsonArray stocks = j["stocks"].to<JsonArray>();
  for (int i = 0; i < g_nStock; i++) stocks.add(g_stocks[i].code);
  JsonArray stas = j["stations"].to<JsonArray>();
  for (int i = 0; i < g_nStation; i++) {
    JsonObject o = stas.add<JsonObject>();
    o["n"] = g_stations[i].name;
    o["u"] = g_stations[i].url;
  }
  j["cur"] = g_curStation;
  j["volume"] = g_volume;
  j["bright"] = g_bright;
  j["autooff"] = g_autoOffMin;            // 分钟档
  j["autopoweroff"] = g_autoPowerOff;
  j["poweroffmin"]  = g_powerOffMin;
  j["refresh"] = g_refreshSec;
  j["jump"] = g_jumpSec;
  j["disp"] = g_displayMode;
  j["gyro"] = g_gyroOn;
  j["alton"] = g_alertOn;
  j["altpct"] = g_alertPct;
  // v7.1: 两套时间范围 / 自动切换间隔 / 每股独立阈值
  JsonArray fw = j["fetchwin"].to<JsonArray>();
  fw.add(g_fetchWin.amS); fw.add(g_fetchWin.amE);
  fw.add(g_fetchWin.pmS); fw.add(g_fetchWin.pmE);
  JsonArray aw = j["alertwin"].to<JsonArray>();
  aw.add(g_alertWin.amS); aw.add(g_alertWin.amE);
  aw.add(g_alertWin.pmS); aw.add(g_alertWin.pmE);
  j["twkd"]  = g_workdayOnly;
  j["ftout"] = g_fetchOutside;
  j["aswsec"] = g_autoSwitchSec;
  JsonArray per = j["altper"].to<JsonArray>();
  for (int i = 0; i < g_nStock; i++) per.add(g_alertPer[i]);
  j["ssid"] = g_wifiSsid;
  // 历史 WiFi: 只回传 SSID 和"是否开放", 密码绝不出现在 HTTP 响应里
  JsonArray wps = j["wifiprofiles"].to<JsonArray>();
  for (int i = 0; i < g_wifiProfileN; i++) {
    JsonObject o = wps.add<JsonObject>();
    o["s"] = g_wifiProfiles[i].ssid;
    o["o"] = (g_wifiProfiles[i].pass[0] == 0);
  }
  j["wifiProfileN"] = (int)g_wifiProfileN;
  j["version"] = FW_VERSION;
  j["wifiState"] = (int)g_wifiState;
  j["wifiStateText"] = wifiStateText();
  j["wifiIp"] = g_wifiIp;
  j["wifiRc"] = g_wifiRc;
  j["wifiRcText"] = (g_wifiState == WIFI_AP_ONLY && g_wifiRc) ? wifiRcText(g_wifiRc) : "";
  j["apIp"] = WiFi.softAPIP().toString();
  j["rssi"] = (g_wifiState == WIFI_CONNECTED) ? WiFi.RSSI() : 0;
  j["radioState"] = (int)g_radioState;
  j["radioHasAudio"] = g_radioHasAudio;
  j["bitrate"] = g_radioBitrate;
  j["codecOk"] = g_codecOk;

  char info[224];
  uint32_t up = millis() / 1000;
#if ENABLE_RADIO
  snprintf(info, sizeof(info),
           "固件 %s\n网络 %s\nIP %s\n热点 192.168.4.1\n空闲内存 %uKB\n已运行 %luh%02lum\n"
           "音频 %s %u kbps\n电台状态 %d",
           FW_VERSION, wifiStateText(), g_wifiIp,
           (unsigned)(ESP.getFreeHeap() / 1024), up / 3600, (up / 60) % 60,
           g_codecOk ? "OK" : "未应答", (unsigned)(g_radioBitrate / 1000),
           (int)g_radioState);
#else
  snprintf(info, sizeof(info),
           "固件 %s\n网络 %s\nIP %s\n热点 192.168.4.1\n空闲内存 %uKB\n已运行 %luh%02lum\n"
           "亮度 %u%% · 音量 %u · 熄屏 %u分 · 关机 %s\n信息更新/预警提示时段见设置页",
           FW_VERSION, wifiStateText(), g_wifiIp,
           (unsigned)(ESP.getFreeHeap() / 1024), up / 3600, (up / 60) % 60,
           (unsigned)g_bright, (unsigned)g_volume,
           (unsigned)g_autoOffMin, poffName(g_powerOffMin));
#endif
  j["info"] = info;
  resp->setLength();
  r->send(resp);
}

// ---------------- 诊断接口: 运行时状态(串口抓不到日志时用网页看) ----------------
static void handleDiag(AsyncWebServerRequest* r) {
  AsyncJsonResponse* resp = new AsyncJsonResponse(false);
  JsonVariant j = resp->getRoot();

  j["fw"] = FW_VERSION;
  j["heap"] = (unsigned)(ESP.getFreeHeap() / 1024);
  j["psram"] = (unsigned)(ESP.getPsramSize() >> 20);

  // WiFi
  j["wifiState"] = (int)g_wifiState;
  j["wifiStateText"] = wifiStateText();
  j["wifiIp"] = g_wifiIp;
  j["rssi"] = (g_wifiState == WIFI_CONNECTED) ? WiFi.RSSI() : 0;

  // 音频 codec(ES8311)
  j["codecOk"]   = g_codecOk;
  j["codecAddr"] = codecI2CAddr();
  j["reg32"]     = codecVolumeReg();   // 0x32 DAC 音量: 0=静音 255=最大, 0xFF=codec 不在线
  j["volume"]    = (int)g_volume;
  j["spkEn"]     = digitalRead(PIN_SPK_EN);

  // 重力感应
  j["gyroOn"] = g_gyroOn;
  j["gyroPresent"] = gyroPresent();
  j["gyroAddr"] = gyroAddr();
  j["gyroOrient"] = (int)gyroRead();
  // I2C 总线扫描: 看总线上到底挂了哪些设备(定位 IMU 真身)
  {
    uint8_t devs[32];
    int nd = gyroI2CScan(devs, 32);
    JsonArray i2c = j["i2cDevs"].to<JsonArray>();
    for (int i = 0; i < nd; i++) {
      char h[8];
      snprintf(h, sizeof(h), "0x%02X", devs[i]);
      i2c.add(h);
    }
  }

  // IMU 探测明细(每个应答地址的 WHO_AM_I 读值, 用于确认板上是否有六轴)
  j["imuProbe"] = gyroProbeText();

  // 按键: 组合值 + 三个引脚各自电平
  //   L/R: 1=未按 0=按下 | M: 0=未按 1=按下(官方高电平有效)
  j["btnRaw"] = btnRawState();
  j["btnPinL"] = digitalRead(PIN_BTN_L);
  j["btnPinM"] = digitalRead(PIN_BTN_M);
  j["btnPinR"] = digitalRead(PIN_BTN_R);
  // 电平跳变计数: 按几下 M 再看, 不变=引脚没读到, 增加=采样正常
  j["btnEdgeL"] = btnEdgeCount(0);
  j["btnEdgeM"] = btnEdgeCount(1);
  j["btnEdgeR"] = btnEdgeCount(2);
  // GPIO0..21 原始电平快照(按住 M 时查一次, 与未按状态对比可定位实际引脚)
  {
    String g;
    for (int p = 0; p <= 21; p++) g += (digitalRead(p) ? '1' : '0');
    j["gpio0to21"] = g;
  }

  // 电池(GPIO1 ADC / GPIO48 充电状态); 用官方 power_manager.h 标定表
  {
    int raw = analogRead(PIN_BAT_VSEN);
    j["batRaw"] = raw;
    j["batPct"] = batPctFromRaw(raw);
    j["batMv"]  = batMvFromRaw(raw);   // 仅近似, 供参考
    j["charging"] = (digitalRead(PIN_CHG_STATUS) == LOW);
  }

  // 提示音(全部是设备本机状态, 与电脑/浏览器无关)
  j["beepCalls"]  = beepCallCount();
  j["beepErr"]    = beepLastErr();
  j["beepActive"] = beepActive();
  j["beepWritten"] = beepWrittenBytes();  // 0=没写进 I2S
  j["beepRet"]    = beepWriteRet();       // 0=ESP_OK

  // 显示 / 电源 / 提醒状态
  j["dispMode"]     = (int)g_displayMode;
  j["autoOffMin"]   = (int)g_autoOffMin;
  j["autoPowerOff"] = (int)g_autoPowerOff;
  j["powerOffMin"]  = (int)g_powerOffMin;
  j["screenOn"]     = (bool)g_screenOn;
  j["alertActive"]  = uiAlertActive();

  // ---- v7.1: 两套时间窗口 / 提醒阈值 / 自动关机计时 / 自动显示 ----
  {
    struct tm t;
    bool hasTime = getLocalTime(&t, 50);
    j["hasTime"]  = hasTime;
    j["now"]      = hasTime ? (int)(t.tm_hour * 60 + t.tm_min) : -1;
    j["weekday"]  = hasTime ? (int)t.tm_wday : -1;
  }
  j["inFetchWin"] = isTradingTime();
  j["inAlertWin"] = isAlertTime();
  j["workdayOnly"]  = (int)g_workdayOnly;
  j["fetchOutside"] = (int)g_fetchOutside;
  {
    JsonArray fw = j["fetchWin"].to<JsonArray>();
    fw.add(g_fetchWin.amS); fw.add(g_fetchWin.amE);
    fw.add(g_fetchWin.pmS); fw.add(g_fetchWin.pmE);
    JsonArray aw = j["alertWin"].to<JsonArray>();
    aw.add(g_alertWin.amS); aw.add(g_alertWin.amE);
    aw.add(g_alertWin.pmS); aw.add(g_alertWin.pmE);
  }
  j["alertOn"]   = (int)g_alertOn;
  j["alertPct"]  = g_alertPct;
  {
    JsonArray th = j["alertEffective"].to<JsonArray>();
    for (int i = 0; i < g_nStock; i++) th.add(alertThresholdFor(i));
  }
  j["autoSwitchSec"] = (int)g_autoSwitchSec;
  j["autoEffMode"]   = (int)g_autoEffMode;
  // 自动关机倒计时: base=实际采用的计时基准, left=距关机剩余秒(-1=未启用/无基准)
  {
    uint32_t base = uiScreenOffAt() ? uiScreenOffAt() : uiLastTouchAt();
    uint32_t due  = (uint32_t)g_powerOffMin * 60000UL;
    uint32_t el   = base ? (millis() - base) : 0;
    j["offBase"]  = (unsigned)base;
    j["offDueMs"] = (unsigned)due;
    j["offLeftS"] = (g_powerOffMin > 0 && g_autoPowerOff && base)
                      ? (int)((el >= due) ? 0 : (due - el) / 1000UL) : -1;
    j["screenOffAt"] = (unsigned)uiScreenOffAt();
    j["lastTouchAt"] = (unsigned)uiLastTouchAt();
  }

  // 股票
  j["stockCount"] = (int)g_nStock;
  int validN = 0;
  if (quotesLock(50)) {
    for (int i = 0; i < g_nStock; i++)
      if (g_quotes[i].valid) validN++;
    quotesUnlock();
  }
  j["stockValid"] = validN;

  resp->setLength();
  r->send(resp);
}

static void handleStocks(AsyncWebServerRequest* r, String& body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) { sendOk(r, false, "JSON解析失败"); return; }
  JsonArray codes = doc["codes"];
  if (codes.isNull() || codes.size() == 0) { sendOk(r, false, "列表为空"); return; }
  if (codes.size() > MAX_STOCKS) { sendOk(r, false, "最多 30 只"); return; }
  StockDef tmp[MAX_STOCKS];
  memset(tmp, 0, sizeof(tmp));
  int n = 0;
  for (const char* c : codes) {
    String cs(c);
    cs.trim(); cs.toLowerCase();
    cs = normalizeCode(cs);       // 纯 6 位数字 -> sh/sz/bj 前缀
    if (!validCode(cs)) {
      String e = "代码格式不对: " + cs;
      sendOk(r, false, e.c_str());
      return;
    }
    strncpy(tmp[n].code, cs.c_str(), STK_CODE_LEN - 1);
    n++;
  }
  g_nStock = n;
  memcpy(g_stocks, tmp, sizeof(tmp));
  g_curStock = 0;
  if (quotesLock()) {
    for (int i = 0; i < MAX_STOCKS; i++) g_quotes[i].valid = false;
    quotesUnlock();
  }
  cfgSaveLists();
  stockRequestRefresh();
  uiRedrawTicker();
  uiInvalidate();
  sendOk(r, true);
}

static void handleStations(AsyncWebServerRequest* r, String& body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) { sendOk(r, false, "JSON解析失败"); return; }
  JsonArray stas = doc["stations"];
  if (stas.isNull() || stas.size() == 0) { sendOk(r, false, "列表为空"); return; }
  if (stas.size() > MAX_STATIONS) { sendOk(r, false, "最多 8 个"); return; }
  Station tmp[MAX_STATIONS];
  memset(tmp, 0, sizeof(tmp));
  int n = 0;
  for (JsonObject o : stas) {
    String nm = String((const char*)(o["n"] | ""));
    String url = String((const char*)(o["u"] | ""));
    nm.trim(); url.trim();
    if (!url.length()) {
      String e = "第 " + String(n + 1) + " 个电台缺少播放地址";
      sendOk(r, false, e.c_str());
      return;
    }
    if (url.length() >= STA_URL_LEN) { sendOk(r, false, "地址过长"); return; }
    if (!url.startsWith("http://") && !url.startsWith("https://")) {
      String e = "第 " + String(n + 1) + " 个地址需以 http:// 开头";
      sendOk(r, false, e.c_str());
      return;
    }
    if (nm.length() >= STA_NAME_LEN) { sendOk(r, false, "名称过长(最多13汉字)"); return; }
    if (!nm.length()) nm = "电台" + String(n + 1);
    strncpy(tmp[n].name, nm.c_str(), STA_NAME_LEN - 1);
    strncpy(tmp[n].url, url.c_str(), STA_URL_LEN - 1);
    n++;
  }
  g_nStation = n;
  memcpy(g_stations, tmp, sizeof(tmp));
  int cur = doc["cur"] | 0;
  g_curStation = constrain(cur, 0, n - 1);
  cfgSaveLists();
  uiFlashRadioLine();
  uiInvalidate();
  radioPlay(g_curStation);
  sendOk(r, true);
}

static void handleSettings(AsyncWebServerRequest* r, String& body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) { sendOk(r, false, "JSON解析失败"); return; }

  // 密码先校验, 不合法就整体拒绝(以前是"静默忽略", 用户以为改成功了)
  String newPass;
  if (!doc["adminpass"].isNull()) {
    newPass = String((const char*)doc["adminpass"]);
    if (newPass.length() < 4 || newPass.length() > 15) {
      sendOk(r, false, "密码需 4-15 位");
      return;
    }
  }
  if (!doc["refresh"].isNull())  g_refreshSec = constrain((int)doc["refresh"], 3, 120);
  if (!doc["jump"].isNull())     g_jumpSec    = constrain((int)doc["jump"], 2, 60);
  if (!doc["bright"].isNull()) {
    g_bright = constrain((int)doc["bright"], 10, 100);
    uiApplyBrightness();
  }
  if (!doc["autooff"].isNull()) {
    int a = (int)doc["autooff"];
    // 只允许 0/1/3/5/10/30/60/120 分钟档
    switch (a) {
      case 0: case 1: case 3: case 5: case 10: case 30: case 60: case 120: break;
      default: a = 0;
    }
    g_autoOffMin = (uint8_t)a;
    uiTouch();
  }
  if (!doc["poweroffmin"].isNull()) {
    int p = (int)doc["poweroffmin"];
    bool ok = false;
    for (int i = 0; i < POFF_N; i++) if (POFF_STEPS[i] == p) { ok = true; break; }
    g_powerOffMin  = ok ? (uint8_t)p : 30;   // 只接受 0/30/60/120/240
    g_autoPowerOff = g_powerOffMin ? 1 : 0;  // 与盒子菜单/旧字段保持同步
  } else if (!doc["autopoweroff"].isNull()) {
    // 兼容旧版网页: 只给开关时保留原分钟数或回落 30 分
    g_autoPowerOff = ((int)doc["autopoweroff"]) ? 1 : 0;
    g_powerOffMin  = g_autoPowerOff ? (g_powerOffMin ? g_powerOffMin : 30) : 0;
  }
  if (!doc["disp"].isNull()) {
    int d = (int)doc["disp"];
    g_displayMode = (d >= DISP_ONE && d <= DISP_AUTO) ? d : DISP_LIST;
  }
  if (!doc["gyro"].isNull())  g_gyroOn = ((int)doc["gyro"]) ? 1 : 0;
  if (!doc["alton"].isNull()) g_alertOn = ((int)doc["alton"]) ? 1 : 0;
  if (!doc["altpct"].isNull()) {
    float p = (float)doc["altpct"];
    if (p < 1.0f) p = 1.0f;
    if (p > 20.0f) p = 20.0f;
    g_alertPct = p;
  }
  // ---- v7.1: 两套独立时间范围 [上午起,上午止,下午起,下午止] 单位=分钟 ----
  if (doc["fetchwin"].is<JsonArray>()) {
    JsonArray a = doc["fetchwin"].as<JsonArray>();
    if (a.size() >= 4) {
      g_fetchWin = { (uint16_t)constrain((int)a[0], 0, 1439),
                     (uint16_t)constrain((int)a[1], 0, 1439),
                     (uint16_t)constrain((int)a[2], 0, 1439),
                     (uint16_t)constrain((int)a[3], 0, 1439) };
      cfgSanitizeWindow(g_fetchWin);
    }
  }
  if (doc["alertwin"].is<JsonArray>()) {
    JsonArray a = doc["alertwin"].as<JsonArray>();
    if (a.size() >= 4) {
      g_alertWin = { (uint16_t)constrain((int)a[0], 0, 1439),
                     (uint16_t)constrain((int)a[1], 0, 1439),
                     (uint16_t)constrain((int)a[2], 0, 1439),
                     (uint16_t)constrain((int)a[3], 0, 1439) };
      cfgSanitizeWindow(g_alertWin);
    }
  }
  if (!doc["twkd"].isNull())  g_workdayOnly  = ((int)doc["twkd"]) ? 1 : 0;
  if (!doc["ftout"].isNull()) g_fetchOutside = ((int)doc["ftout"]) ? 1 : 0;
  if (!doc["aswsec"].isNull()) {
    int v = constrain((int)doc["aswsec"], 0, 120);
    if (v != 0 && v != 10 && v != 20 && v != 30 && v != 60 && v != 120) v = 30;
    g_autoSwitchSec = (uint8_t)v;
  }
  // 每股独立阈值: 0=跟随全局 / 1..20=独立 / -1=该股禁用
  if (doc["altper"].is<JsonArray>()) {
    JsonArray a = doc["altper"].as<JsonArray>();
    for (int i = 0; i < MAX_STOCKS && i < (int)a.size(); i++) {
      float v = (float)a[i];
      if (v < 0) v = -1.0f;
      else if (v > 0) { if (v < 1.0f) v = 1.0f; if (v > 20.0f) v = 20.0f; }
      g_alertPer[i] = v;
    }
  }
  if (!doc["volume"].isNull()) {
    g_volume = constrain((int)doc["volume"], 0, 100);
#if ENABLE_RADIO
    radioSetVolume(g_volume);
#endif
    uiShowVolume();
  }
  if (newPass.length()) strcpy(g_adminPass, newPass.c_str());

  cfgSaveSettings();
  // ★ 网页改设置"提示已保存但盒子没反应"的主因: 若屏幕处于自动熄屏态,
  //   uiTask 会 continue 跳过 s_dirty 消费, 改动永远不会画出来。
  //   这里主动点亮 + 重置熄屏计时 + 给设备端可见反馈, 保证即时生效。
  uiTouch();
  if (!g_screenOn) uiSetScreen(true);
  uiInvalidate();
  uiToast("设置已生效", 0x05BF, 1200);
  sendOk(r, true);
}

static void handleRadio(AsyncWebServerRequest* r, String& body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) { sendOk(r, false, "JSON解析失败"); return; }
  const char* cmd = doc["cmd"] | "";
  if (!strcmp(cmd, "vol")) {
    radioSetVolume((int)(doc["vol"] | g_volume));
    uiShowVolume();
  } else if (!strcmp(cmd, "toggle")) {
    radioTogglePlay(); uiFlashRadioLine(); uiTouch();
  } else if (!strcmp(cmd, "stop")) {
    radioStop(); uiFlashRadioLine(); uiInvalidate(); uiTouch();
  } else if (!strcmp(cmd, "next")) {
    radioNextStation(); uiFlashRadioLine(); uiTouch();
  } else if (!strcmp(cmd, "play")) {
    radioPlay((int)(doc["idx"] | g_curStation)); uiFlashRadioLine();
  }
  sendOk(r, true);
}

// 提示音自检: 网页点一下 -> 盒子板载喇叭播"滴滴"(不会让电脑发声)
//   固定 90% 音量, 且出声前重刷 ES8311 的 0x32 音量寄存器。
static void handleBeep(AsyncWebServerRequest* r, String& body) {
  (void)body;
  beepTest();
  uiToast("提示音测试", 0x05BF, 1200);
  sendOk(r, true);
}

static bool applyWifi(const char* ssid, const char* pass) {
  if (!ssid || !strlen(ssid) || strlen(ssid) > 32) return false;
  strncpy(g_wifiSsid, ssid, 32); g_wifiSsid[32] = 0;
  strncpy(g_wifiPass, pass ? pass : "", 64); g_wifiPass[64] = 0;
  cfgSaveSettings();
  cfgWifiAdd(g_wifiSsid, g_wifiPass);   // 记住这组凭据(空密码=开放网络), 之后换地点自动连
  wifiApplyCreds();          // 立即热切换, 不重启(本板重启会掉电源锁存)
  uiInvalidate();
  uiTouch();
  return true;
}

static void handleWifi(AsyncWebServerRequest* r, String& body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) { sendOk(r, false, "JSON解析失败"); return; }
  const char* ssid = doc["ssid"] | "";
  const char* pass = doc["pass"] | "";
  if (!applyWifi(ssid, pass)) { sendOk(r, false, "WiFi 名称无效(1-32 字符)"); return; }
  sendOk(r, true);
}

// ---- v7.0: 历史 WiFi 切换/删除 + 行情刷新 + 重新联网 ----
static void handleWifiSel(AsyncWebServerRequest* r, String& body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) { sendOk(r, false, "JSON解析失败"); return; }
  int idx = doc["idx"] | -1;
  if (!cfgWifiUse(idx)) { sendOk(r, false, "序号无效"); return; }
  wifiApplyCreds();
  uiInvalidate();
  uiTouch();
  sendOk(r, true);
}

static void handleWifiDel(AsyncWebServerRequest* r, String& body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) { sendOk(r, false, "JSON解析失败"); return; }
  int idx = doc["idx"] | -1;
  if (!cfgWifiDel(idx)) { sendOk(r, false, "序号无效"); return; }
  sendOk(r, true);
}

static void handleRefresh(AsyncWebServerRequest* r, String& body) {
  (void)body;
  stockRequestRefresh();
  uiInvalidate();
  sendOk(r, true);
}

static void handleReconnect(AsyncWebServerRequest* r, String& body) {
  (void)body;
  wifiReconnect();
  sendOk(r, true);
}

void webLoopHandler() {
  wifiLoop();
  if (s_powerOffAt && millis() > s_powerOffAt) { s_powerOffAt = 0; powerOff(); }
  if (s_otaDone) { s_otaDone = false; delay(400); ESP.restart(); }

  // v7.1 严格 I2C 探测(112 个地址逐一带二次 ACK + 寄存器双读, 约 1~3 秒)
  if (s_imuReq) {
    s_imuReq = false;
    String out;
    int n = gyroFullScan(out, (int)s_imuPwr);
    s_imuOut = out;
    s_imuN   = n;
    s_imuDone = true;
    Serial.printf("[IMU] 严格扫描完成 n=%d %s\n", n, out.c_str());
  }

  // WiFi 扫描放在这里(主循环), 不占用 async_tcp 任务, 否则页面会转圈
  if (s_scanReq) {
    s_scanReq = false;
    String ssid[20];
    int rssi[20], enc[20];
    int n = wifiScan(ssid, rssi, enc, 20);
    String err, list;
    if (n < 0) {
      // 已连接状态也允许扫描(可随时换 WiFi), 只有真的失败才报错
      err  = "扫描失败,请重试";
      list = "[]";
    } else {
      list = "[";
      for (int i = 0; i < n; i++) {
        if (i) list += ',';
        list += "{\"s\":\"" + jsonEsc(ssid[i]) + "\",\"r\":" + String(rssi[i]) +
                ",\"e\":" + String(enc[i]) + "}";
      }
      list += "]";
    }
    if (s_scanMux && xSemaphoreTake(s_scanMux, pdMS_TO_TICKS(300)) == pdTRUE) {
      s_scanJson = list;
      s_scanErr  = err;
      s_scanDone = true;
      xSemaphoreGive(s_scanMux);
    }
    Serial.printf("[WiFi] 扫描完成 n=%d\n", n);
  }
}

void webBegin() {
  if (!s_scanMux) s_scanMux = xSemaphoreCreateMutex();
  uint32_t rnd = esp_random();
  s_token = String(rnd, HEX) + String(esp_random(), HEX);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (isAuthed(r)) r->send_P(200, "text/html", PAGE_MAIN);
    else r->send_P(200, "text/html", PAGE_LOGIN);
  });

  server.on("/login", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (r->hasParam("pass", true)) {
      String pass = r->getParam("pass", true)->value();
      if (pass == g_adminPass) {
        AsyncWebServerResponse* resp = r->beginResponse(303);
        resp->addHeader("Set-Cookie", "box0tok=" + s_token + "; Path=/; HttpOnly");
        resp->addHeader("Location", "/");
        r->send(resp);
        return;
      }
    }
    r->send_P(200, "text/html",
      "<!DOCTYPE html><html><head><meta charset=utf-8>"
      "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
      "<style>body{background:#111;color:#eee;font-family:sans-serif;text-align:center;padding-top:80px}"
      "a{color:#1d9e75}</style></head><body>"
      "<p>密码错误</p><p><a href=/>重试</a></p></body></html>");
  });

  server.on("/logout", HTTP_GET, [](AsyncWebServerRequest* r) {
    AsyncWebServerResponse* resp = r->beginResponse(303);
    resp->addHeader("Set-Cookie", "box0tok=; Path=/; Max-Age=0");
    resp->addHeader("Location", "/");
    r->send(resp);
  });

  server.on("/api/cfg", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!isAuthed(r)) { r->send(401); return; }
    handleCfg(r);
  });

  server.on("/api/diag", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!isAuthed(r)) { r->send(401); return; }
    handleDiag(r);
  });

  // 统一约定: POST /api/xxx?d=<urlencode(json)>
  server.on("/api/stocks",   HTTP_POST, _post(handleStocks),   nullptr, collectBody);
  server.on("/api/stations", HTTP_POST, _post(handleStations), nullptr, collectBody);
  server.on("/api/settings", HTTP_POST, _post(handleSettings), nullptr, collectBody);
  server.on("/api/radio",    HTTP_POST, _post(handleRadio),    nullptr, collectBody);
  server.on("/api/beep",     HTTP_POST, _post(handleBeep),     nullptr, collectBody);
  server.on("/api/wifi",     HTTP_POST, _post(handleWifi),     nullptr, collectBody);
  server.on("/api/wifisel",  HTTP_POST, _post(handleWifiSel),  nullptr, collectBody);
  server.on("/api/wifidel",  HTTP_POST, _post(handleWifiDel),  nullptr, collectBody);
  server.on("/api/refresh",  HTTP_POST, _post(handleRefresh),  nullptr, collectBody);
  server.on("/api/reconnect",HTTP_POST, _post(handleReconnect),nullptr, collectBody);

  // ---- WiFi 状态 / 地址栏救急配网 ----
  // http://192.168.4.1/api/wifi?ssid=家里WiFi&pass=密码&pw=123456
  server.on("/api/wifi", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!isAuthedOrPw(r)) { r->send(401); return; }
    if (r->hasParam("ssid")) {
      bool ok = applyWifi(qs(r, "ssid").c_str(), qs(r, "pass").c_str());
      Serial.printf("[Web] 地址栏配网 ok=%d ssid=%s\n", ok, g_wifiSsid);
    }
    AsyncJsonResponse* resp = new AsyncJsonResponse();
    JsonVariant j = resp->getRoot();
    j["wifiState"]     = (int)g_wifiState;
    j["wifiStateText"] = wifiStateText();
    j["wifiIp"]        = g_wifiIp;
    j["apIp"]          = WiFi.softAPIP().toString();
    j["wifiRcText"]    = (g_wifiState == WIFI_AP_ONLY && g_wifiRc) ? wifiRcText(g_wifiRc) : "";
    j["rssi"]          = (g_wifiState == WIFI_CONNECTED) ? WiFi.RSSI() : 0;
    resp->setLength();
    r->send(resp);
  });

  // ---- 扫描 ----
  server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!isAuthed(r)) { r->send(401); return; }
    if (s_scanMux && xSemaphoreTake(s_scanMux, pdMS_TO_TICKS(100)) == pdTRUE) {
      s_scanDone = false;
      s_scanErr  = "";
      xSemaphoreGive(s_scanMux);
    }
    s_scanReq = true;
    sendOk(r, true);
  });

  server.on("/api/scanres", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!isAuthed(r)) { r->send(401); return; }
    String out = "{\"done\":false}";
    if (s_scanMux && xSemaphoreTake(s_scanMux, pdMS_TO_TICKS(200)) == pdTRUE) {
      out = "{\"done\":";
      out += s_scanDone ? "true" : "false";
      if (s_scanDone) {
        out += ",\"err\":\"" + jsonEsc(s_scanErr) + "\",\"list\":";
        out += s_scanJson;
      }
      out += "}";
      xSemaphoreGive(s_scanMux);
    }
    r->send(200, "application/json", out);
  });

  // ---- v7.1 严格姿态传感器探测(验证板上到底有没有加速度计) ----
  //  触发: GET /api/imu[?pwr=空闲GPIO]  读取: GET /api/imu?get=1
  //  扫描放主循环, 立即回 started, 页面轮询结果, 不阻塞 async_tcp
  server.on("/api/imu", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!isAuthed(r)) { r->send(401); return; }
    AsyncJsonResponse* resp = new AsyncJsonResponse();
    JsonVariant j = resp->getRoot();
    if (r->hasParam("get")) {
      j["done"]    = (bool)s_imuDone;
      j["count"]   = s_imuDone ? s_imuN : 0;
      j["devices"] = s_imuDone ? s_imuOut : String("");
      j["note"]    = "每个地址需连续两次 ACK + 0x00/0x0F/0x75 寄存器双读一致; 空列表=板上无 IMU";
    } else {
      s_imuDone = false;
      s_imuOut  = "";
      s_imuN    = 0;
      s_imuPwr  = r->hasParam("pwr") ? qs(r, "pwr").toInt() : -1;
      s_imuReq  = true;
      j["started"] = true;
      j["poll"]    = "/api/imu?get=1";
    }
    resp->setLength();
    r->send(resp);
  });

  // ---- 关机 ----
  server.on("/api/power", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!isAuthed(r)) { r->send(401); return; }
    s_powerOffAt = millis() + 800;   // 先回响应, 再断电
    sendOk(r, true);
  });

  // ---- 恢复出厂 ----
  server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!isAuthed(r)) { r->send(401); return; }
    cfgFactoryReset();
    g_curStock = 0;
    WiFi.disconnect(true, false);
    wifiReconnect();                 // 无 SSID -> 保持热点模式
    uiRedrawTicker();
    uiInvalidate();
    uiToast("已恢复出厂", 0xFD20, 3000);
    sendOk(r, true);
  });

  // ---- 网页 OTA: 手机连热点打开 http://192.168.4.1/ota 上传 .bin 即可升级 ----
  //      有了它, 以后改固件不用再插 USB、不用再折腾下载模式
  server.on("/ota", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!isAuthedOrPw(r)) { r->send(401); return; }   // 升级页必须鉴权
    static const char PAGE_OTA[] PROGMEM =
      "<!doctype html><meta charset=utf-8>"
      "<meta name=viewport content='width=device-width,initial-scale=1'>"
      "<style>body{font-family:sans-serif;padding:18px;background:#111;color:#eee}"
      "input,button{font-size:16px;margin:10px 0;width:100%;padding:10px}"
      "button{background:#2d7;color:#000;border:0;border-radius:6px}</style>"
      "<h3>BOX0 固件升级</h3>"
      "<form method=POST action='/api/ota' enctype='multipart/form-data'>"
      "<input type=file name=f accept='.bin' required>"
      "<button type=submit>上传并升级</button></form>"
      "<p style='color:#888;font-size:13px'>选择 .bin 应用镜像(不是 merged)。"
      "上传完成后设备自动重启, 期间不要断电。</p>";
    r->send_P(200, "text/html", PAGE_OTA);
  });

  server.on("/api/ota", HTTP_POST,
    [](AsyncWebServerRequest* r) {
      if (!isAuthedOrPw(r)) { r->send(401); return; }
      bool ok = !Update.hasError();
      r->send(ok ? 200 : 500, "text/plain", ok ? "OK, rebooting" : "FAIL");
      if (ok) s_otaDone = true;
    },
    [](AsyncWebServerRequest* r, String filename, size_t index,
       uint8_t* data, size_t len, bool final) {
      if (!isAuthedOrPw(r)) return;          // 未授权时一个字节都不写进 flash
      if (!index) {
        Serial.printf("[OTA] 开始 %s\n", filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
      }
      if (len && Update.write(data, len) != len) Update.printError(Serial);
      if (final) {
        if (!Update.end(true)) Update.printError(Serial);
        else Serial.println("[OTA] 写入完成");
      }
    });

  server.onNotFound([](AsyncWebServerRequest* r) {
    r->redirect("/");
  });
  server.begin();
  Serial.println("[Web] HTTP server on port 80");
}
