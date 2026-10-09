#include "webui.h"

#include "bench.h"
#include "timing.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include <Arduino.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>

#include <cstring>

namespace birdposter {

namespace {

WebServer server(80);
DNSServer dns;
volatile bool touched = false;

// %KEY% tokens are substituted by render(). Kept plain: nothing *needs* a
// script, because a phone on a captive-portal sheet may not run one. The
// script at the bottom only adds conveniences - hiding the fields the chosen
// source ignores, a place lookup, a timezone picker, live progress after
// "fetch and draw" - and without it every field shows and the form still
// works.
const char kPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bird poster</title>
<style>
:root{color-scheme:light;--ink:#26231d;--muted:#6f6858;--line:#e3dccb;--field:#cfc7b3;--paper:#f4f1ea;--green:#2a5c2a}
body{font:16px/1.5 system-ui,sans-serif;margin:0;padding:1rem;max-width:40rem;margin-inline:auto;background:var(--paper);color:var(--ink)}
h1{font:italic 2rem Georgia,serif;margin:.3rem 0 .8rem}
h2{font-size:.82rem;text-transform:uppercase;letter-spacing:.08em;color:var(--green);margin:0 0 .2rem}
h3{font-size:.78rem;text-transform:uppercase;letter-spacing:.07em;color:var(--muted);margin:1.1rem 0 0;padding-top:.9rem;border-top:1px solid var(--line)}
fieldset{border:0;padding:0;margin:0}
.card{position:relative;background:#fff;border:1px solid var(--line);border-radius:10px;padding:1rem 1rem 1.1rem;margin:0 0 .9rem;box-shadow:0 1px 2px rgba(60,50,20,.05)}
label{display:block;margin:.6rem 0 .2rem;font-size:.88rem;color:#4a4538}
input,select{width:100%;box-sizing:border-box;padding:.5rem .6rem;border:1px solid var(--field);border-radius:6px;font:inherit;background:#fff;color:var(--ink)}
input:focus,select:focus{outline:2px solid #9cc29c;outline-offset:0;border-color:var(--green)}
input[type=checkbox]{width:1.1rem;height:1.1rem;margin:0;accent-color:var(--green);flex:none}
.row{display:flex;gap:.7rem}.row>*{flex:1;min-width:0}.row>button{flex:none!important}
.check{display:flex;align-items:center;gap:.5rem;margin:.75rem 0 .1rem}
.check>label{display:flex;align-items:center;gap:.55rem;margin:0;font-size:.95rem;color:var(--ink);cursor:pointer}
.status{display:grid;grid-template-columns:auto 1fr;gap:.15rem 1rem;font-size:.92rem}
.status div{display:contents}.status b{font-weight:600;color:var(--muted)}
.bad{color:#b02020}
img.preview{display:block;width:100%;height:auto;border-radius:6px;margin-top:.9rem;border:1px solid var(--line)}
button{font:inherit;padding:.55rem 1rem;border-radius:6px;border:1px solid var(--field);background:#fff;color:var(--ink);cursor:pointer}
button:hover{background:#f7f4ec}
button.primary{background:var(--green);color:#fff;border-color:var(--green);font-weight:600}
button.primary:hover{background:#234d23}
button.danger{color:#a3271d;border-color:#e1b9b3}
.actions{display:flex;flex-wrap:wrap;align-items:center;gap:.5rem;margin-top:.9rem}
.actions form{margin:0}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:.5rem}.grid form{margin:0}.grid button{width:100%;height:100%}.grid .wide{grid-column:1/-1}
.savebar{position:sticky;bottom:0;z-index:4;padding:.7rem 0 .9rem;margin-top:-.2rem;background:linear-gradient(rgba(244,241,234,0),var(--paper) 35%)}
.savebar button{width:100%;padding:.75rem;box-shadow:0 2px 8px rgba(42,92,42,.25)}
small{color:var(--muted)}
#quietnote{display:block;margin-top:.5rem}
[hidden]{display:none!important}
.busy{background:#fff8e1;border:1px solid #e0c060;border-radius:10px;padding:.7rem 1rem;margin:0 0 .9rem}
.places{display:flex;flex-wrap:wrap;gap:.4rem;margin-top:.4rem}
.places button{font-size:.85rem;padding:.3rem .6rem}
.lookerr{display:block;color:#a3271d;background:#fbe9e7;border:1px solid #d9a09a;border-radius:6px;padding:.45rem .7rem;font-size:.88rem}
.err{background:#fbe9e7;border:1px solid #d9a09a;border-radius:10px;padding:.7rem 1rem;margin:0 0 .9rem}
.spin{display:inline-block;width:.9em;height:.9em;border:2px solid #999;border-top-color:var(--green);border-radius:50%;vertical-align:-.15em;margin-right:.4em;animation:spin .8s linear infinite}
@keyframes spin{to{transform:rotate(360deg)}}
button:disabled{opacity:.6;cursor:wait}
.info{display:inline-block;flex:none}
.info>summary{list-style:none;cursor:pointer;width:1.05rem;height:1.05rem;border:1.5px solid #a39c88;border-radius:50%;color:#7a7360;font:italic 700 .72rem/1 Georgia,serif;display:flex;align-items:center;justify-content:center;user-select:none}
.info>summary::-webkit-details-marker{display:none}
.info[open]>summary{background:var(--green);border-color:var(--green);color:#fff}
.info>div{position:absolute;left:.6rem;right:.6rem;z-index:5;margin-top:.4rem;background:#fffdf7;border:1px solid var(--field);border-radius:8px;padding:.65rem .85rem;box-shadow:0 6px 18px rgba(0,0,0,.14);font-size:.88rem;line-height:1.45;color:#444}
.lh{display:flex;align-items:center;gap:.4rem;margin:.6rem 0 .2rem}.lh>label{margin:0}
@font-face{font-family:BPName;src:url(/font/name.ttf)}@font-face{font-family:BPLabel;src:url(/font/label.ttf)}
.nameprev{position:relative;margin-top:.8rem;border:1px solid var(--line);border-radius:8px;padding:1.6rem .8rem 1.1rem;text-align:center;color:#111;line-height:1;overflow:hidden;white-space:nowrap}
.npcap{position:absolute;top:.35rem;left:.6rem;font:600 .68rem system-ui,sans-serif;letter-spacing:.07em;text-transform:uppercase;color:var(--muted)}
.linebox{border:1px solid var(--line);border-radius:8px;background:#fbf9f4;padding:.2rem .8rem .8rem;margin-top:.7rem}.linehead{font-weight:600;font-size:.9rem;margin-top:.5rem}
.tpcap{margin-top:.8rem;font:600 .68rem system-ui,sans-serif;letter-spacing:.07em;text-transform:uppercase;color:var(--muted)}
.textprev{position:relative;margin-top:.25rem;border:1px solid var(--line);border-radius:8px;display:flex;flex-direction:column;overflow:hidden;color:#111;line-height:1;background:#fff}
.tpband{display:grid;grid-template-columns:1fr auto 1fr;align-items:baseline;white-space:nowrap;gap:.3em}.tpband>span:nth-child(2){text-align:center}.tpband>span:nth-child(3){text-align:right}
.tpgap{height:1rem}
.examples{list-style:none;padding:0;margin:.3rem 0 0}.examples li{display:flex;flex-wrap:wrap;align-items:center;gap:.2rem .5rem;padding:.35rem 0;border-top:1px solid var(--line)}.tokens .examples code{flex:1 1 100%;font-size:.82rem;user-select:all;white-space:normal;overflow-wrap:anywhere}.exout{flex:1;font-family:BPLabel,Georgia,serif;font-style:italic;color:var(--ink)}.exout:before{content:"\2192  ";font-style:normal;color:var(--muted)}.examples em{flex:1 1 100%;font-size:.78rem;color:var(--muted)}.examples button{padding:.15rem .55rem;font-size:.8rem;margin-left:.3rem}.tokens h4{margin:.8rem 0 0;font-size:.78rem;text-transform:uppercase;letter-spacing:.07em;color:var(--muted)}
.tokens{margin:.7rem 0 0;font-size:.85rem;color:#444}.tokens summary{cursor:pointer;color:var(--green)}.tokens table{border-collapse:collapse;margin:.4rem 0;width:100%}.tokens td code{white-space:normal}.tokens td,.tokens th{padding:.2rem .4rem .2rem 0;vertical-align:top;border-top:1px solid var(--line);text-align:left}.tokens th{font-size:.72rem;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);border-top:0}.tokens code{font-size:.8rem;white-space:nowrap}
#np1{font-family:BPName,Georgia,serif}#np2{font-family:BPLabel,Georgia,serif;font-style:italic}
.credits{margin:1.2rem 0 .4rem;font-size:.85rem;color:var(--muted)}.credits summary{cursor:pointer}.credits p{margin:.4rem 0}
footer{font-size:.85rem;color:var(--muted);margin:.6rem 0 1.5rem}
</style></head><body>
<h1>Bird poster</h1>
<div class="card">
<div class="status">
<div><b>Network</b><span>%NET%</span></div>
<div><b>Last fetch</b><span class="%FETCHCLASS%">%FETCH%</span></div>
<div><b>On the glass</b><span>%GLASS%</span></div>
<div><b>Rendered</b><span>%RENDERED%</span></div>
<div><b>Next</b><span>%NEXT%</span></div>
<div><b>Plates</b><span>%PLATES%</span></div>
<div><b>Firmware</b><span>%VERSION%</span></div>
</div>
%PREVIEW%
</div>
<div id="busy" class="busy" %BUSYSHOW%><b>Working:</b> <span id="phase">%PHASE%</span><br><small>About a minute all told; the glass flashes at the end. This page updates itself when it is done.</small></div>
%ERROR%

<form method="post" action="/wifi" class="card">
<h2>WiFi</h2>
<label>Networks nearby</label><div class="row"><select id="nearby" onchange="if(this.value)document.getElementsByName('ssid')[0].value=this.value"><option value="">%SCANNOTE%</option>%NETWORKS%</select><button type="submit" formaction="/action" formmethod="post" name="do" value="scan" formnovalidate style="flex:0;white-space:nowrap">Scan again</button></div>
<label>Network name (SSID)</label><input name="ssid" value="%SSID%" maxlength="32" required list="ssids" autocomplete="off"><datalist id="ssids">%SSIDLIST%</datalist>
<label>Password</label><input name="pass" type="password" minlength="8" maxlength="63" title="8 to 63 characters, or blank" placeholder="%PASSHINT%" autocomplete="off">
<div class="actions"><button class="primary" type="submit">Save WiFi and join</button></div>
</form>

<form method="post" action="/frame" class="card">
<h2>Frame</h2>
<div class="row" style="align-items:flex-end"><div><div class="lh"><label>Hostname</label><details class="info"><summary title="More about this">i</summary><div>The frame answers at http://%HOST%.local/ on your network. A new name is used from the next time the frame joins.</div></details></div><input name="host" value="%HOST%" maxlength="24" pattern="[a-z0-9]([a-z0-9-]*[a-z0-9])?" title="lower-case letters, digits and hyphens, not starting or ending with a hyphen" autocapitalize="off"></div>
<div><div class="lh"><label>Setup network password</label><details class="info"><summary title="More about this">i</summary><div>The password for the network the frame makes itself when it has no WiFi to join. It is not shown here; leave it blank to keep what is saved.</div></details></div><input name="appass" type="password" minlength="8" maxlength="63" placeholder="unchanged" autocomplete="off" title="at least 8 characters"></div></div>
<div class="actions"><button class="primary" type="submit">Save</button></div>
</form>



<form method="post" action="/save" id="settings">
%OFFLINE_NOTE%
<div class="card">
<h2>Bird Source</h2>
<label>Source</label><select name="source" id="source" onchange="srcChanged()"><option value="inat" %SRC_INAT%>iNaturalist - what people record nearby</option><option value="ebird" %SRC_EBIRD%>eBird - what birders report nearby (needs a free key)</option><option value="ala" %SRC_ALA%>Atlas of Living Australia - every record near here</option><option value="birdnet" %SRC_BN%>BirdNET-Go - local detections</option><option value="list" %SRC_LIST%>A JSON list of names at a URL of your own</option></select>
<div class="lh"><label>Layout order</label><details class="info"><summary title="More about this">i</summary><div>Which birds make the page and in what order: the first takes the middle (with Hero, it is the hero) and the rest fill out from there. <b>Most seen</b> ranks by how many sightings or detections each species has in the window. <b>Rarest in the world</b> ranks by how few times a species has been recorded anywhere, so the unusual visitor leads; iNaturalist and eBird only. On eBird, "rarest" is its <i>notable</i> list: sightings eBird's own regional filters flag as unusual for the place and the season.</div></details></div><select name="mode" id="mode"><option value="most" %MODE_MOST%>Most seen</option><option value="rarest" id="rarest" %MODE_RARE%>Rarest in the world</option></select>
<div class="birdnet"><label>BirdNET-Go address</label><input name="detector" value="%DETECTOR%" maxlength="256" placeholder="http://birdnet-go.local:8080"></div>
<div class="birdnet"><div class="lh"><label>Minimum confidence (%)</label><details class="info"><summary title="More about this">i</summary><div>Detections BirdNET-Go is less sure of than this are left out, as if they had not been heard. 0 takes everything the detector kept - it has its own threshold already, so this only ever raises the bar.</div></details></div><input name="bnconf" type="number" min="0" max="100" value="%BNCONF%"></div>
<div class="birdnet"><div class="check"><label><input type="checkbox" name="newfirst" value="1" %NEWON%>New birds first</label><details class="info"><summary title="More about this">i</summary><div>A bird is new on the day BirdNET-Go first ever hears it - the detector marks those detections itself - and stays new until midnight, as BirdNET-Go's own "new" badge does. New birds go on the page ahead of the rest, the first of them in the middle, even when the layout order would have left them off, and stay on it all that day even if they are not heard again. Needs BirdNET-Go from July 2025 or later; an older one never marks a bird new, and the page is as without this.</div></details></div><input type="hidden" name="newfirst" value="0"></div>
<div class="list"><div class="lh"><label>List URL</label><details class="info"><summary title="More about this">i</summary><div>Anything that answers with JSON: a bare list of scientific names, <code>["Turdus merula", &hellip;]</code>, or a list of objects with <code>scientific</code>, and optionally <code>common</code> and <code>count</code>. BirdNET-Go's own field names work too. The order given is the ranking when there are no counts.</div></details></div><input name="listurl" value="%LISTURL%" maxlength="256" placeholder="http://homeassistant.local:8123/local/birds.json"></div>
<div class="ebird"><div class="lh"><label>eBird API key</label><details class="info"><summary title="More about this">i</summary><div>Free and instant from <a href="https://ebird.org/api/keygen" target="_blank">ebird.org/api/keygen</a> with an eBird account. The frame only reads with it. Once saved it is never shown again, here or anywhere; leave the field blank to keep it, or type a new one to replace it. eBird looks back 30 days at most and 50 km at most.</div></details></div><input name="ebirdkey" id="ebirdkey" value="" maxlength="64" placeholder="%EBIRDHINT%" data-saved="%EBIRDSAVED%" autocomplete="off">
<label>Names in</label><select name="ebirdloc"><option value="en_AU" %EBL_AU%>Australian English - Grey Teal, Australian Wood Duck</option><option value="en" %EBL_EN%>Clements English - Gray Teal, Maned Duck</option><option value="en_NZ" %EBL_NZ%>New Zealand English</option><option value="en_UK" %EBL_UK%>British English</option><option value="en_IN" %EBL_IN%>Indian English</option><option value="en_ZA" %EBL_ZA%>South African English</option></select></div>
<div class="where">
<div class="js online" %OFFLINE%><label>Lookup location</label><div class="row"><input id="place" placeholder="Sydney" autocomplete="off"><button type="button" onclick="findPlace()" style="flex:0;white-space:nowrap">Look up</button></div><div class="places" id="places"></div></div>
<div class="row"><div><label>Latitude</label><input name="lat" id="lat" type="number" step="any" min="-90" max="90" value="%LAT%"></div><div><label>Longitude</label><input name="lng" id="lng" type="number" step="any" min="-180" max="180" value="%LNG%"></div></div>
<small class="wxnote">Only for the weather on the page: this source is not asked about a place.</small>
</div>
<div class="place">
<div class="row"><div><label>Radius (<span id="radunit">%DISTNAME%</span>)</label><input name="radius" id="radius" type="number" min="1" max="%RADIUSMAX%" value="%RADIUS%"></div>
<div class="inat"><div class="lh"><label>iNaturalist API</label><details class="info inat"><summary title="More about this">i</summary><div>v2 sends only what the frame reads (about an eighth of the bytes) but iNaturalist may still change it; v1 is frozen and sends everything, so a fetch takes longer. If v2 stops working, switch.</div></details></div><select name="inatv"><option value="2" %INATV2%>v2 - lean, may change</option><option value="1" %INATV1%>v1 - stable, slower</option></select></div></div>
</div>
<div class="window"><div class="lh"><label>Look back</label><details class="info"><summary title="More about this">i</summary><div>Which sightings count: the top birds seen in this window. BirdNET-Go is asked for its last 200 detections (1000 with "every bird" on) and the window is applied to those.</div></details></div><div class="row"><select name="lookbackunit" id="lookbackunit" onchange="lbChanged()"><option value="0" %LB0%>minutes</option><option value="1" %LB1%>hours</option><option value="2" %LB2%>days</option><option value="3" %LB3%>Since last update</option></select><input name="lookback" id="lookback" type="number" min="1" max="10000" value="%LOOKBACK%"></div></div>
<div class="online" %OFFLINE%><div class="actions"><button type="submit" formaction="/test" formnovalidate id="testbtn">Test source</button><span id="testing" hidden><span class="spin"></span>Asking the source&hellip;</span><details class="info"><summary title="More about this">i</summary><div>Asks the source with the values above, saved or not, and says what came back. A few seconds usually; up to 30 if nothing answers at the address.</div></details></div></div>
</div>

<div class="card">
<h2>Layout</h2>
%PACKS%
<div class="row" style="align-items:flex-end"><div><label>Max birds on the page (limit: 40)</label><input name="birds" id="birds" type="number" min="1" max="40" value="%BIRDS%"></div>
<div><label>Orientation</label><select name="rotation"><option value="1" %ROT1%>Landscape</option><option value="3" %ROT3%>Landscape, flipped</option><option value="0" %ROT0%>Portrait</option><option value="2" %ROT2%>Portrait, flipped</option></select></div></div>
<div class="birdnet"><div class="check"><label><input type="checkbox" name="everybird" id="everybird" value="1" %EVERYON% onchange="srcChanged()">Every bird detected in the look-back window</label><details class="info"><summary title="More about this">i</summary><div>Instead of a set number, the page holds every species BirdNET-Go heard in the window, up to the number above (the most heard, if more). With the window set to "Since last update", each page is exactly what was heard since the one before; if nothing was, the last page stays up and the window keeps growing until something is.</div></details></div><input type="hidden" name="everybird" value="0"></div>
<div class="lh"><label>Arrangement</label><details class="info"><summary title="More about this">i</summary><div>Classic packs the birds into the centre of the page at one size. Grid and Scattered start them evenly apart and let each grow into the room beside it, which fills a busy page harder and draws the birds at more than one size. Hero gives the middle of the page to the first bird the source ranked, sets its name larger to match, and rings the others around it.</div></details></div><select name="packstyle"><option value="0" %PKS0%>Classic - a cluster from the middle out</option><option value="1" %PKS1%>Grid - evenly spaced, grown to fit</option><option value="2" %PKS2%>Scattered - evenly spread, no rows</option><option value="3" %PKS3%>Hero - one bird large, the rest around it</option></select>
<div class="lh"><label>Don't repeat a bird for</label><details class="info"><summary title="More about this">i</summary><div>0 is off: the page draws the most seen (or rarest) every time. Above 0, each page is a random pick of the birds not drawn in that long, so the frame works through everything seen nearby before repeating. A year at most.</div></details></div><div class="row"><select name="cycleunit" id="cycleunit"><option value="1" %CYU1%>hours</option><option value="24" %CYU24%>days</option><option value="168" %CYU168%>weeks</option></select><input name="cycle" id="cycle" type="number" min="0" max="8760" value="%CYCLE%"></div>
<div class="check"><label><input type="checkbox" name="shuffle" value="1" %SHUFON%>Shuffle the birds' order</label><details class="info"><summary title="More about this">i</summary><div>The same birds, handed to the layout in a new random order every page, rather than the most seen (or rarest) first. The first bird takes the middle of the page, so this moves which bird gets it - with Hero, which bird is the hero.</div></details></div><input type="hidden" name="shuffle" value="0">

<h3>Names</h3>
<div class="row"><div><label>Show</label><select name="names"><option value="0" %NAMES0%>Both</option><option value="1" %NAMES1%>Just scientific</option><option value="2" %NAMES2%>Just common</option><option value="3" %NAMES3%>None</option></select></div>
<div><label>Common name case</label><select name="namecase"><option value="1" %CASE1%>ALL CAPS</option><option value="0" %CASE0%>As given</option><option value="2" %CASE2%>lower case</option></select></div></div>
<div class="row"><div><label>Size</label><select name="label"><option value="0" %LBL0%>Small</option><option value="1" %LBL1%>Medium</option><option value="2" %LBL2%>Large</option><option value="3" %LBL3%>Extra large</option></select></div>
<div><label>Scientific name size</label><select name="scipct"><option value="100" %SCI100%>100% (same)</option><option value="90" %SCI90%>90%</option><option value="80" %SCI80%>80%</option><option value="70" %SCI70%>70%</option><option value="60" %SCI60%>60%</option><option value="50" %SCI50%>50% (half)</option></select></div></div>

<div class="birdnet"><div class="check"><label><input type="checkbox" name="showconf" id="showconf" value="1" %CONFON%>Show BirdNET-Go's confidence after each name</label><details class="info"><summary title="More about this">i</summary><div>Written after the first line of each name, e.g. SUPERB FAIRYWREN (87%): the surest of that bird's detections in the look-back window. In the common names' capitals face it needs the font from this firmware's filesystem image for the %; with an older one the name is set in the label face instead.</div></details></div><input type="hidden" name="showconf" value="0"></div>
<div class="js" hidden><div class="nameprev" id="nameprev"><span class="npcap">Preview</span><div id="np1"></div><div id="np2"></div></div></div>
</div>

<div class="card">
<h2>Page text</h2>
<div class="lh"><label>Your own lines along the top and bottom</label><details class="info"><summary title="More about this">i</summary><div>At the name size, shrunk to fit if long. The birds are packed clear of any edge with text on it. For the date, put <code>{{date.long}}</code> or another of the templates below in a line.</div></details></div>
<div class="linebox"><div class="linehead">Top line</div><label>Text</label><input name="toptext" value="%TOPTEXT%" maxlength="120" placeholder="e.g. Seen near home, {{date.long}}" autocomplete="off">
<div class="row"><div><div class="lh"><label>Size</label><details class="info"><summary title="More about this">i</summary><div>Small to Extra large match the name sizes above; Huge is larger, for a heading. A line too long for the page at its size is shrunk to fit. The preview below is drawn to scale: its width stands for the whole width of the glass.</div></details></div><select name="topsize"><option value="1" %TSZ1%>Small</option><option value="2" %TSZ2%>Medium</option><option value="3" %TSZ3%>Large</option><option value="4" %TSZ4%>Extra large</option><option value="5" %TSZ5%>Huge</option></select></div>
<div><label>Alignment</label><select name="topalign"><option value="0" %TAL0%>Left</option><option value="1" %TAL1%>Centre</option><option value="2" %TAL2%>Right</option></select></div>
</div>
<div class="row"><div><div class="lh"><label>Font</label><details class="info"><summary title="More about this">i</summary><div>The label face is the italic the scientific names are set in. The name face is the common names' capitals: a line in it is set in capitals, and one with a character it lacks goes in the label face instead. A new-bird line is set in the face of the line it stands in for.</div></details></div><select name="topface"><option value="0" %TFC0%>Label (italic)</option><option value="1" %TFC1%>Name (capitals)</option></select></div><div></div></div>
</div>
<div class="linebox"><div class="linehead">Bottom line</div><label>Text</label><input name="bottomtext" value="%BOTTEXT%" maxlength="120" placeholder="e.g. Updated {{time}}, next at {{next}}" autocomplete="off">
<div class="row"><div><label>Size</label><select name="bottomsize"><option value="1" %BSZ1%>Small</option><option value="2" %BSZ2%>Medium</option><option value="3" %BSZ3%>Large</option><option value="4" %BSZ4%>Extra large</option><option value="5" %BSZ5%>Huge</option></select></div>
<div><label>Alignment</label><select name="bottomalign"><option value="0" %BAL0%>Left</option><option value="1" %BAL1%>Centre</option><option value="2" %BAL2%>Right</option></select></div>
</div>
<div class="row"><div><label>Font</label><select name="bottomface"><option value="0" %BFC0%>Label (italic)</option><option value="1" %BFC1%>Name (capitals)</option></select></div><div></div></div>
</div>
<div class="birdnet"><div class="check"><label><input type="checkbox" name="newtext" id="newtext" value="1" %NEWTEXTON% onchange="newTextChanged()">Different lines when a bird is new</label><details class="info"><summary title="More about this">i</summary><div>When the page has a bird BirdNET-Go calls a new species today, these lines are used instead - all day, as BirdNET-Go keeps its badge. <code>{{new}}</code> names the new birds. Leave one empty to keep the usual line on that edge.</div></details></div><input type="hidden" name="newtext" value="0">
<div id="newtextopts"><div class="row"><div><label>Top line, new bird</label><input name="newtoptext" value="%NEWTOP%" maxlength="120" placeholder="e.g. New today: {{new}}!" autocomplete="off"></div>
<div><label>Bottom line, new bird</label><input name="newbottomtext" value="%NEWBOT%" maxlength="120" placeholder="(the usual bottom line)" autocomplete="off"></div></div></div></div>
<details class="tokens"><summary>Templates</summary><div>Anything in double braces is filled in when the page is drawn. A line that asks for something the frame does not have - the clock not yet set, a battery on a board that cannot read one - is left off the page rather than printed with a gap.<table><tr><th>Tag</th><th>Description</th><th>Example</th></tr><tr><td><code>{{date.<wbr>long}}</code></td><td>Today&#39;s date, the month in full</td><td>26 September 2026</td></tr><tr><td><code>{{date.<wbr>medium}}</code></td><td>Today&#39;s date, the month shortened</td><td>26 Sep 2026</td></tr><tr><td><code>{{date.<wbr>short}}</code></td><td>Today&#39;s date in figures, two-digit year, in the date order under Preferences</td><td>26/09/26</td></tr><tr><td><code>{{date.<wbr>numeric}}</code></td><td>Today&#39;s date in figures, full year, in the date order under Preferences</td><td>26/09/2026</td></tr><tr><td><code>{{date.<wbr>full}}</code></td><td>Today&#39;s date with the day of the week</td><td>Saturday 26 September 2026</td></tr><tr><td><code>{{time}}</code></td><td>The time the page was drawn, on the clock under Preferences</td><td>14:05 or 2:05 pm</td></tr><tr><td><code>{{time.<wbr>24h}}</code></td><td>The time the page was drawn, always 24-hour</td><td>14:05</td></tr><tr><td><code>{{time.<wbr>12h}}</code></td><td>The time the page was drawn, always 12-hour</td><td>2:05 pm</td></tr><tr><td><code>{{hour.<wbr>24h}}</code></td><td>The hour the page was drawn, always 24-hour</td><td>14</td></tr><tr><td><code>{{hour.<wbr>12h}}</code></td><td>The hour the page was drawn, always 12-hour</td><td>2 pm</td></tr><tr><td><code>{{weekday}}</code></td><td>Today&#39;s day of the week</td><td>Saturday</td></tr><tr><td><code>{{weekday.<wbr>short}}</code></td><td>Today&#39;s day of the week, shortened</td><td>Sat</td></tr><tr><td><code>{{day}}</code></td><td>Today&#39;s day of the month</td><td>26</td></tr><tr><td><code>{{month}}</code></td><td>This month&#39;s name</td><td>September</td></tr><tr><td><code>{{month.<wbr>short}}</code></td><td>This month&#39;s name, shortened</td><td>Sep</td></tr><tr><td><code>{{month.<wbr>number}}</code></td><td>This month as a number</td><td>09</td></tr><tr><td><code>{{year}}</code></td><td>This year</td><td>2026</td></tr><tr><td><code>{{next}}</code></td><td>When the next page is due, after any quiet hours, on the clock under Preferences</td><td>15:05 or 3:05 pm</td></tr><tr><td><code>{{next.<wbr>24h}}</code></td><td>When the next page is due, after any quiet hours, always 24-hour</td><td>15:05</td></tr><tr><td><code>{{next.<wbr>12h}}</code></td><td>When the next page is due, after any quiet hours, always 12-hour</td><td>3:05 pm</td></tr><tr><td><code>{{birds}}</code></td><td>How many birds are on the page</td><td>10</td></tr><tr><td><code>{{top}}</code></td><td>The common name of the source&#39;s first-ranked bird on the page, before any shuffle</td><td>Superb Fairywren</td></tr><tr><td><code>{{top.<wbr>scientific}}</code></td><td>The scientific (Latin) name of the source&#39;s first-ranked bird on the page, before any shuffle</td><td>Malurus cyaneus</td></tr><tr><td><code>{{new}}</code></td><td>The birds on the page BirdNET-Go calls new species today</td><td>Galah and Crimson Rosella</td></tr><tr><td><code>{{new.<wbr>count}}</code></td><td>How many birds on the page BirdNET-Go calls new species today</td><td>2</td></tr><tr><td><code>{{weather.<wbr>now}}</code></td><td>The temperature when the page was drawn</td><td>14&deg;</td></tr><tr><td><code>{{weather.<wbr>summary}}</code></td><td>The weather when the page was drawn</td><td>Partly cloudy</td></tr><tr><td><code>{{weather.<wbr>today}}</code></td><td>Today&#39;s forecast</td><td>Light rain</td></tr><tr><td><code>{{weather.<wbr>high}}</code></td><td>Today&#39;s forecast high</td><td>18&deg;</td></tr><tr><td><code>{{weather.<wbr>low}}</code></td><td>Today&#39;s forecast low</td><td>6&deg;</td></tr><tr><td><code>{{weather.<wbr>rain}}</code></td><td>Today&#39;s forecast chance of rain</td><td>60%</td></tr><tr><td><code>{{weather.<wbr>tomorrow}}</code></td><td>Tomorrow&#39;s forecast</td><td>Overcast</td></tr><tr><td><code>{{weather.<wbr>tomorrow.<wbr>high}}</code></td><td>Tomorrow&#39;s forecast high</td><td>21&deg;</td></tr><tr><td><code>{{weather.<wbr>tomorrow.<wbr>low}}</code></td><td>Tomorrow&#39;s forecast low</td><td>9&deg;</td></tr><tr><td><code>{{weather.<wbr>tomorrow.<wbr>rain}}</code></td><td>Tomorrow&#39;s forecast chance of rain</td><td>10%</td></tr><tr><td><code>{{source}}</code></td><td>Where the birds came from</td><td>iNaturalist</td></tr><tr><td><code>{{window}}</code></td><td>The time the birds were seen in, or &quot;since the last update&quot;</td><td>last 7 days</td></tr><tr><td><code>{{place}}</code></td><td>The latitude and longitude searched, place-based sources only</td><td>-35.2809, 149.1300</td></tr><tr><td><code>{{radius}}</code></td><td>The distance searched, in the unit under Preferences, place-based sources only</td><td>25 km</td></tr><tr><td><code>{{refresh}}</code></td><td>How many times the glass has been refreshed, since the count was last cleared under Preferences</td><td>123</td></tr><tr><td><code>{{battery}}</code></td><td>The battery&#39;s voltage, reTerminal E1004 only</td><td>3.92 V</td></tr><tr><td><code>{{battery.<wbr>percent}}</code></td><td>The battery&#39;s charge, a rough guide from its voltage, reTerminal E1004 only</td><td>78%</td></tr></table>The weather is from <a href="https://open-meteo.com" target="_blank">Open-Meteo</a> (free, no key) for the latitude and longitude above, in the unit under Preferences, fetched with the birds only when a line asks for it. A line with the time, the weather, the next update, the refresh count or the battery in it changes every time, so the page is redrawn at every refresh rather than kept when nothing else has changed.<h4>Examples</h4><ul class="examples"><li><code>Seen near home, {{date.long}}</code><span class="exout">Seen near home, 26 September 2026</span><span><button type="button" data-to="toptext">Top</button><button type="button" data-to="bottomtext">Bottom</button></span></li><li><code>{{weekday}} {{date.long}}, drawn at {{time}}</code><span class="exout">Saturday 26 September 2026, drawn at 2:05 pm</span><span><button type="button" data-to="toptext">Top</button><button type="button" data-to="bottomtext">Bottom</button></span></li><li><code>{{birds}} species, {{window}}</code><span class="exout">10 species, last 7 days</span><span><button type="button" data-to="toptext">Top</button><button type="button" data-to="bottomtext">Bottom</button></span></li><li><code>Bird of the day: {{top}}</code><span class="exout">Bird of the day: Superb Fairywren</span><span><button type="button" data-to="toptext">Top</button><button type="button" data-to="bottomtext">Bottom</button></span></li><li class="birdnet"><code>New today: {{new}}!</code><span class="exout">New today: Galah and Crimson Rosella!</span> <em>(BirdNET-Go, for the new-bird lines)</em><span><button type="button" data-to="newtoptext">New-bird top</button><button type="button" data-to="newbottomtext">New-bird bottom</button></span></li><li><code>{{weather.now}}, {{weather.summary}}, high of {{weather.high}}</code><span class="exout">14°, Partly cloudy, high of 18°</span><span><button type="button" data-to="toptext">Top</button><button type="button" data-to="bottomtext">Bottom</button></span></li><li><code>Today: {{weather.today}}, {{weather.high}}/{{weather.low}}, {{weather.rain}} chance of rain</code><span class="exout">Today: Light rain, 18°/6°, 60% chance of rain</span><span><button type="button" data-to="toptext">Top</button><button type="button" data-to="bottomtext">Bottom</button></span></li><li><code>Tomorrow: {{weather.tomorrow}}, {{weather.tomorrow.high}}/{{weather.tomorrow.low}}</code><span class="exout">Tomorrow: Overcast, 21°/9°</span><span><button type="button" data-to="toptext">Top</button><button type="button" data-to="bottomtext">Bottom</button></span></li><li><code>Next update {{next}}</code><span class="exout">Next update 3:05 pm</span><span><button type="button" data-to="toptext">Top</button><button type="button" data-to="bottomtext">Bottom</button></span></li><li><code>Within {{radius}} of {{place}} - {{source}}</code><span class="exout">Within 25 km of -35.2809, 149.1300 - iNaturalist</span><span><button type="button" data-to="toptext">Top</button><button type="button" data-to="bottomtext">Bottom</button></span></li><li><code>Battery {{battery.percent}}, refresh {{refresh}}</code><span class="exout">Battery 78%, refresh 123</span> <em>(the battery on the reTerminal E1004 only)</em><span><button type="button" data-to="toptext">Top</button><button type="button" data-to="bottomtext">Bottom</button></span></li></ul></div></details>
<div class="js" hidden><div class="tpcap">Preview</div><div class="textprev" id="textprev"><div class="tpband" id="tptop"><span></span><span></span><span></span></div><div class="tpgap"></div><div class="tpband" id="tpbot"><span></span><span></span><span></span></div></div>
<div class="birdnet"><div class="check"><label><input type="checkbox" id="tpnew" onchange="textPreview()">Preview as a page with a new bird</label></div></div><small id="tpnote"></small></div>
</div>

<div class="card">
<h2>Picture</h2>
<div class="row"><div><div class="lh"><label>Colour</label><details class="info"><summary title="More about this">i</summary><div>The glass has six dull inks, so a plate comes out flatter than it was printed. Colour pushes the saturation back up: 0 leaves the plate as it is, 4 is the most vivid. Pick it by eye.</div></details></div><select name="vivid"><option value="0" %VIV0%>0 (dull)</option><option value="1" %VIV1%>1</option><option value="2" %VIV2%>2</option><option value="3" %VIV3%>3</option><option value="4" %VIV4%>4 (vivid)</option></select></div>
<div><div class="lh"><label>Detail</label><details class="info"><summary title="More about this">i</summary><div>The dither blurs fine lines - an engraving's hatching most of all. Detail sharpens what contrast there is before the dither: 0 is off, 4 the sharpest.</div></details></div><select name="sharpen"><option value="0" %SHP0%>0 (soft)</option><option value="1" %SHP1%>1</option><option value="2" %SHP2%>2</option><option value="3" %SHP3%>3</option><option value="4" %SHP4%>4 (sharp)</option></select></div></div>
<div class="row"><div><div class="lh"><label>Edges</label><details class="info"><summary title="More about this">i</summary><div>Draws a line along every boundary it finds, faint ones included, which is what keeps a white bird off a white page. 0 draws none, 4 the strongest.</div></details></div><select name="edges"><option value="0" %EDG0%>0 (none)</option><option value="1" %EDG1%>1</option><option value="2" %EDG2%>2</option><option value="3" %EDG3%>3</option><option value="4" %EDG4%>4 (strong)</option></select></div>
<div><div class="lh"><label>Paper</label><details class="info"><summary title="More about this">i</summary><div>Prints the page on cream instead of white: the background and the plates' own pale paper take the same warm tone, so the birds sit into the page rather than on it. The glass has no cream ink, so it comes out as a fine stipple of yellow and white.</div></details></div><select name="cream"><option value="0" %CRM0%>White</option><option value="1" %CRM1%>1 (faint)</option><option value="2" %CRM2%>2</option><option value="3" %CRM3%>3</option><option value="4" %CRM4%>4 (warmest)</option></select></div></div>
<div class="row"><div><div class="lh"><label>Margin</label><details class="info"><summary title="More about this">i</summary><div>A border the page draws nothing in, so a mount or a bezel over the glass does not cut the names off the edge. 0 is the glass itself: the birds bleed off it. The page is 1600 x 1200 pixels whichever way the frame hangs, and top is the top of the picture; the birds are packed into what is left, so a margin makes them smaller rather than leaving a gap. At most a quarter of the page a side.</div></details></div><select name="marginmode" id="marginmode" onchange="marginChanged()"><option value="0" %MGM0%>Same all round</option><option value="1" %MGM1%>Each side</option></select></div>
<div id="marginone"><label>All sides (px)</label><input name="margin" type="number" min="0" max="300" value="%MARGIN%"></div></div>
<div class="row" id="marginfour"><div><label>Top</label><input name="margintop" type="number" min="0" max="300" value="%MARGINT%"></div>
<div><label>Right</label><input name="marginright" type="number" min="0" max="300" value="%MARGINR%"></div>
<div><label>Bottom</label><input name="marginbottom" type="number" min="0" max="300" value="%MARGINB%"></div>
<div><label>Left</label><input name="marginleft" type="number" min="0" max="300" value="%MARGINL%"></div></div>
<h3>Extras</h3>
<div class="check"><label><input type="checkbox" name="webplates" id="webplates" value="1" %WEBON% onchange="webChanged()">Pull full-size plates from the web</label><details class="info"><summary title="More about this">i</summary><div>For a bird drawn much larger than its plate in flash - a page of one or two birds - the frame fetches the same plate at full size from here (<code>&lt;address&gt;/Genus_species.bin</code>), and uses the one in flash if the site does not answer. A normal page never needs it. Put <code>{region}</code> in the address for a site with a folder a region. %WEBLAST%</div></details></div><input type="hidden" name="webplates" value="0">
<input name="weburl" id="weburl" value="%WEBURL%" maxlength="256" placeholder="https://c4kew4lk.github.io/bird_poster/plates/{region}" autocomplete="off" style="margin-top:.4rem">
</div>

<div class="card">
<h2>Schedule</h2>
<label>Refresh every (minutes)</label><input name="interval" id="interval" type="number" min="1" max="1440" value="%INTERVAL%" required>
<div class="row"><div><div class="lh"><label>Quiet from</label><details class="info"><summary title="More about this">i</summary><div>No new pages between these times, in the frame's timezone; the keys still work. The same time twice means never quiet.</div></details></div><input name="quietfrom" id="quietfrom" type="time" value="%QFROM%" required pattern="([01]?[0-9]|2[0-3]):[0-5][0-9]" placeholder="22:00"></div>
<div><label>Until</label><input name="quietto" id="quietto" type="time" value="%QTO%" required pattern="([01]?[0-9]|2[0-3]):[0-5][0-9]" placeholder="06:00"></div></div>
<small id="quietnote"></small>
<div class="js" hidden><label>Timezone</label><div class="row"><select id="tzsel" onchange="tzPick()">
<option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Sydney, Canberra, Melbourne, Hobart (AEST/AEDT)</option>
<option value="AEST-10">Brisbane (AEST)</option>
<option value="ACST-9:30ACDT,M10.1.0,M4.1.0/3">Adelaide (ACST/ACDT)</option>
<option value="ACST-9:30">Darwin (ACST)</option>
<option value="AWST-8">Perth (AWST)</option>
<option value="NZST-12NZDT,M9.5.0,M4.1.0/3">New Zealand (NZST/NZDT)</option>
<option value="JST-9">Japan (JST)</option>
<option value="IST-5:30">India (IST)</option>
<option value="GMT0BST,M3.5.0/1,M10.5.0">United Kingdom (GMT/BST), Ireland (GMT/IST)</option>
<option value="CET-1CEST,M3.5.0,M10.5.0/3">Central Europe (CET/CEST)</option>
<option value="EET-2EEST,M3.5.0/3,M10.5.0/4">Eastern Europe (EET/EEST)</option>
<option value="EST5EDT,M3.2.0,M11.1.0">US Eastern (EST/EDT)</option>
<option value="CST6CDT,M3.2.0,M11.1.0">US Central (CST/CDT)</option>
<option value="MST7MDT,M3.2.0,M11.1.0">US Mountain (MST/MDT)</option>
<option value="PST8PDT,M3.2.0,M11.1.0">US Pacific (PST/PDT)</option>
<option value="UTC0">UTC</option>
<option value="">Other - enter it manually</option>
</select><button type="button" id="tzmanual" onclick="tzToggle()" style="flex:0;white-space:nowrap">Manual entry</button></div></div>
<div id="tzbox"><div class="lh"><label>Timezone (POSIX)</label><details class="info"><summary title="More about this">i</summary><div>e.g. AEST-10AEDT,M10.1.0,M4.1.0/3 for Sydney, NZST-12NZDT,M9.5.0,M4.1.0/3, CET-1CEST,M3.5.0,M10.5.0/3, EST5EDT,M3.2.0,M11.1.0</div></details></div><input name="tz" id="tz" value="%TZ%" maxlength="64" placeholder="AEST-10AEDT,M10.1.0,M4.1.0/3"></div>
</div>
<div class="card">
<h2>Preferences</h2>
<div class="row"><div><div class="lh"><label>Dates</label><details class="info"><summary title="More about this">i</summary><div>Which comes first in a short date - <code>{{date.short}}</code> and <code>{{date.numeric}}</code> in the page text. Dates in words are always day first: 26 September 2026.</div></details></div><select name="dateorder"><option value="0" %DOR0%>Day first - 26/09/2026</option><option value="1" %DOR1%>Month first - 09/26/2026</option></select></div>
<div><div class="lh"><label>Distances</label><details class="info"><summary title="More about this">i</summary><div>For the radius the place-based sources search, <code>{{radius}}</code> on the page and the status page. The frame keeps the radius in kilometres, so switching back and forth does not drift it.</div></details></div><select name="distunit" id="distunit" onchange="distChanged()"><option value="0" %DU0%>Kilometres</option><option value="1" %DU1%>Miles</option></select></div></div>
<div class="row"><div><div class="lh"><label>Temperatures</label><details class="info"><summary title="More about this">i</summary><div>For <code>{{weather...}}</code> on the page and the status page.</div></details></div><select name="tempunit"><option value="0" %TU0%>Celsius</option><option value="1" %TU1%>Fahrenheit</option></select></div>
<div><div class="lh"><label>Times</label><details class="info"><summary title="More about this">i</summary><div>For <code>{{time}}</code> and <code>{{next}}</code> on the page, and the times on the status page and here. Their <code>.24h</code> and <code>.12h</code> forms, and the hour tags, keep their own clock whatever this says.</div></details></div><select name="clock"><option value="0" %CK0%>24-hour - 14:05</option><option value="1" %CK1%>12-hour - 2:05 pm</option></select></div></div>
<div class="lh"><label>Refresh count</label><details class="info"><summary title="More about this">i</summary><div>Every refresh of the glass is counted, and <code>{{refresh}}</code> puts the count on the page. Run a charged battery flat and the last number on the glass is how many refreshes it lasted; the count survives the battery going flat. Clear it before a test.</div></details></div>
<div class="row" style="align-items:center"><span>%REFRESHCOUNT%</span><button type="submit" formaction="/action" formmethod="post" name="do" value="resetcount" formnovalidate onclick="return confirm('Clear the refresh count to 0?')">Clear</button></div>
</div>
<div class="savebar"><button class="primary" type="submit">Save settings</button></div>
</form>

<div class="card">
<h2>Actions</h2>
<div class="grid">
<form method="post" action="/action" id="refresh" class="wide"><button name="do" value="refresh" class="primary">Fetch and draw a new page</button></form>
<form method="post" action="/action"><button name="do" value="status">Show status</button></form>
<form method="post" action="/action"><button name="do" value="pattern">Test pattern</button></form>
<form method="post" action="/action"><button name="do" value="setup">Show setup page</button></form>
<form method="post" action="/action"><button name="do" value="sleep">Back to sleep</button></form>
<form method="post" action="/action"><button name="do" value="reboot" class="danger">Reboot</button></form>
<form method="post" action="/action" onsubmit="return confirm('Forget the WiFi network?')"><button name="do" value="forget" class="danger">Forget WiFi</button></form>
</div>
</div>
<details class="credits"><summary>Credits and licences</summary>
<p><b>Artwork.</b> Australian plates: John Gould, <i>The Birds of Australia</i> (1840&ndash;48), lithographed by Elizabeth Gould and H.&nbsp;C. Richter; scans digitally enhanced by <a href="https://www.rawpixel.com/" target="_blank">rawpixel</a>, CC&nbsp;BY-SA&nbsp;4.0, cut for this project under the same licence, with supplementary plates from other public-domain works via <a href="https://commons.wikimedia.org/" target="_blank">Wikimedia Commons</a>, each credited in the style's manifest. European plates: John Gould, <i>The Birds of Europe</i> (1832&ndash;37), and the von Wright brothers, <i>Svenska F&aring;glar</i>; rawpixel-enhanced scans CC&nbsp;BY-SA&nbsp;4.0 and Finnish National Gallery scans CC0; cut-outs by Arne Giacomo Munthe-Kaas for Fugleramme, CC&nbsp;BY-SA&nbsp;4.0. North American plates: John James Audubon, <i>The Birds of America</i> (1827&ndash;38), engraved by Robert Havell; rawpixel-enhanced scans CC&nbsp;BY-SA&nbsp;4.0, cut for this project under the same licence.</p>
<p><b>Names on the page</b> are set in Gentium Book Plus (SIL International) and a display face derived from Playfair Display (Claus Eggers S&oslash;rensen), both under the SIL Open Font License 1.1.</p>
<p><b>Sightings</b> come from BirdNET-Go, <a href="https://www.inaturalist.org/" target="_blank">iNaturalist</a>, <a href="https://ebird.org/" target="_blank">eBird</a> (Cornell Lab of Ornithology) or the <a href="https://www.ala.org.au/" target="_blank">Atlas of Living Australia</a>, as chosen above; species names follow the BirdNET label sets.</p>
<p><b>Code.</b> The firmware is MIT, on <a href="https://github.com/C4KEW4LK/bird_poster" target="_blank">GitHub</a>. It carries ArduinoJson (Beno&icirc;t Blanchon, MIT), stb_truetype (Sean Barrett, public domain), the QR Code generator (Project Nayuki, MIT) and the Arduino core for the ESP32.</p>
</details>
<footer>Keys on the frame: 1 keeps WiFi on for setup, 2 shows the status page, 3 fetches a new page. Drawing a page takes about 40 seconds; the glass flashes while it does.</footer>
<script>
document.querySelectorAll('.js').forEach(function(e){if(!e.classList.contains('online')||%ONLINE%)e.hidden=false});
// One (i) open at a time, and a click anywhere else closes it.
document.addEventListener('click',function(e){document.querySelectorAll('details.info[open]').forEach(function(d){if(!d.contains(e.target))d.open=false})});
function lbChanged(){document.getElementById('lookback').hidden=document.getElementById('lookbackunit').value=='3'}
lbChanged();
function marginChanged(){var four=document.getElementById('marginmode').value=='1';
document.getElementById('marginone').hidden=four;document.getElementById('marginfour').hidden=!four}
marginChanged();
// Options that only matter with their box ticked fold away without it.
function newTextChanged(){document.getElementById('newtextopts').hidden=!document.getElementById('newtext').checked}
newTextChanged();
// The radius box is in the unit picked under Preferences: switching converts what
// is in it, so the search area stays the same.
// The exact distance is kept aside, so going to miles and back gives the
// same kilometres rather than one rounded twice.
var distUnit=document.getElementById('distunit').value,radKm=null;
document.getElementById('radius').addEventListener('input',function(){radKm=null});
function distChanged(){var u=document.getElementById('distunit').value;if(u==distUnit)return;
var r=document.getElementById('radius'),km=1.609344,v=+r.value;
if(r.value!==''&&v>0){if(radKm===null)radKm=distUnit=='1'?v*km:v;r.value=Math.max(1,Math.round(u=='1'?radKm/km:radKm))}
r.max=u=='1'?310:500;document.getElementById('radunit').textContent=u=='1'?'miles':'km';distUnit=u}
function webChanged(){document.getElementById('weburl').hidden=!document.getElementById('webplates').checked}
webChanged();
// The latitude and longitude are wanted by the place-based sources, and by
// the weather whatever the source.
function whereChanged(){var f=document.getElementById('settings').elements,v=document.getElementById('source').value;
var place=v=='inat'||v=='ebird'||v=='ala',wx=['toptext','bottomtext','newtoptext','newbottomtext'].some(function(n){return f[n]&&/\{\{\s*weather\./i.test(f[n].value)});
document.querySelectorAll('.where').forEach(function(e){e.hidden=!(place||wx)});
document.querySelectorAll('.wxnote').forEach(function(e){e.hidden=place})}
function srcChanged(){var v=document.getElementById('source').value;
var show=function(c,on){document.querySelectorAll('.'+c).forEach(function(e){e.hidden=!on})};
whereChanged();
show('birdnet',v=='birdnet');show('list',v=='list');show('ebird',v=='ebird');
show('place',v=='inat'||v=='ebird'||v=='ala');show('inat',v=='inat');show('window',v!='list');
var m=document.getElementById('mode'),r=document.getElementById('rarest'),no=!(v=='inat'||v=='ebird');
r.disabled=no;r.hidden=no;if(no&&m.value=='rarest')m.value='most';
var every=v=='birdnet'&&document.getElementById('everybird').checked;
document.getElementById('cycle').disabled=every;document.getElementById('cycleunit').disabled=every;}
srcChanged();
// The lookup runs from this browser, not the frame: it needs this phone or
// computer to reach iNaturalist. A failure says so rather than hanging - a
// network with no way out often never answers, so it gives up after 8 s.
function findPlace(){var q=document.getElementById('place').value.trim(),out=document.getElementById('places');
if(!q)return;
var fail=function(m){out.className='places lookerr';out.textContent=m};
var offline='No connection to the location service - this phone or computer seems to have no internet. Type the latitude and longitude instead.';
if(navigator.onLine===false){fail(offline);return}
out.className='places';out.textContent='Looking…';
var ac=window.AbortController?new AbortController():null,timer=setTimeout(function(){if(ac)ac.abort()},8000);
fetch('https://api.inaturalist.org/v1/places/autocomplete?per_page=6&q='+encodeURIComponent(q),ac?{signal:ac.signal}:{}).then(function(r){
if(!r.ok)throw{status:r.status};return r.json()}).then(function(j){clearTimeout(timer);
out.textContent='';if(!j.results||!j.results.length){out.textContent='Nothing called that.';return}
j.results.forEach(function(p){if(!p.location)return;var b=document.createElement('button');b.type='button';b.textContent=p.display_name;
b.onclick=function(){var ll=p.location.split(',');document.getElementById('lat').value=(+ll[0]).toFixed(5);document.getElementById('lng').value=(+ll[1]).toFixed(5);out.textContent='Set to '+p.display_name};
out.appendChild(b)})}).catch(function(e){clearTimeout(timer);
fail(e&&e.status?'The location service answered with an error (HTTP '+e.status+'). Try again later, or type the latitude and longitude.':offline)})}
document.getElementById('place').addEventListener('keydown',function(e){if(e.key=='Enter'){e.preventDefault();findPlace()}});
document.getElementById('settings').addEventListener('submit',function(e){var b=e.submitter;if(!b)return;
if(b.id!='testbtn'){check();if(!this.reportValidity()){e.preventDefault();return}}
if(b.id=='testbtn'){document.getElementById('testing').hidden=false;setTimeout(function(){b.disabled=true},0)}
else if(b.type=='submit'){b.innerHTML='<span class="spin"></span>'+b.textContent;setTimeout(function(){b.disabled=true},0)}});
window.addEventListener('pageshow',function(){document.getElementById('testing').hidden=true;document.querySelectorAll('button:disabled').forEach(function(b){b.disabled=false;b.textContent=b.textContent})});
var tz=document.getElementById('tz'),sel=document.getElementById('tzsel'),tzbox=document.getElementById('tzbox'),tzbtn=document.getElementById('tzmanual');
sel.value=tz.value;if(sel.value!==tz.value)sel.value='';
function tzShow(on){tzbox.hidden=!on;tzbtn.textContent=on?'Hide manual entry':'Manual entry'}
tzShow(sel.value==='');
function tzPick(){if(sel.value){tz.value=sel.value;tzShow(false)}else tzShow(true)}
function tzToggle(){tzShow(tzbox.hidden)}
tz.addEventListener('input',function(){sel.value=tz.value;if(sel.value!==tz.value)sel.value=''});
function mins(v){var m=/^(\d{1,2}):(\d{2})/.exec(v||'');if(!m||+m[1]>23||+m[2]>59)return -1;return m[1]*60+ +m[2]}
function hm(n){return Math.floor(n/60)+' h'+(n%60?' '+n%60+' min':'')}
// The checks the server makes too, said before the round trip. Only fields
// that are showing are questioned: a hidden invalid field would block the
// submit with nowhere to say why.
function check(){var f=document.getElementById('settings'),v=document.getElementById('source').value;
var g=function(n){return f.elements[n]},vis=function(e){return e.offsetParent!==null},say=function(e,m){e.setCustomValidity(vis(e)?m:'')};
var lat=g('lat'),lng=g('lng'),place=v=='inat'||v=='ebird'||v=='ala';
say(lat,place&&lat.value.trim()===''?'Enter the latitude, or use the place lookup.':'');
say(lng,place&&lng.value.trim()===''?'Enter the longitude, or use the place lookup.':'');
if(place&&lat.value!==''&&lng.value!==''&&+lat.value==0&&+lng.value==0)say(lat,'0, 0 is the Gulf of Guinea - use the place lookup or type the frame\'s location.');
else if(v=='ala'&&lat.validity.valid&&lng.validity.valid&&lat.value!==''&&lng.value!==''&&(+lat.value<-56||+lat.value>-8||+lng.value<104||+lng.value>170))say(lat,'The Atlas of Living Australia only covers Australia and its territories.');
var key=g('ebirdkey'),k=key.value.trim();say(key,v!='ebird'?'':!k&&key.dataset.saved!='1'?'eBird needs an API key - free from ebird.org/api/keygen.':!k?'':!/^[A-Za-z0-9-]+$/.test(k)?'The key is only letters, digits and dashes - check it was copied whole.':'');
var url=/^https?:\/\/\S+$/i;
say(g('detector'),v=='birdnet'&&!url.test(g('detector').value.trim())?'Start the address with http:// or https://':'');
say(g('listurl'),v=='list'&&!url.test(g('listurl').value.trim())?'Start the URL with http:// or https://':'');
var w=g('weburl');say(w,g('webplates')[0].checked&&!url.test(w.value.trim())?'Start the address with http:// or https://':'');
var qf=g('quietfrom'),qt=g('quietto'),a=mins(qf.value),b=mins(qt.value),iv=+g('interval').value,note=document.getElementById('quietnote');
say(qf,a<0?'Enter a time such as 22:00':'');say(qt,b<0?'Enter a time such as 06:00':'');
if(a>=0&&b>=0){var q=(b-a+1440)%1440;
if(a==b)note.textContent='Never quiet: a new page every '+(iv||'?')+' minutes, day and night.';
else if(1440-q<Math.max(iv,60)){say(qt,'That leaves under '+hm(Math.max(iv,60))+' awake a day - the frame would hardly draw. Shorten the quiet time.');note.textContent=''}
else note.textContent='Quiet for '+hm(q)+' a day, from '+qf.value+' to '+qt.value+(b<a?' the next morning':'')+'; a new page every '+(iv||'?')+' minutes the rest of the time.';}
var tz=g('tz'),t=tz.value.trim();say(tz,t&&!(t.length<=64&&/^([A-Za-z]{3,}|<[^>]+>)[+-]?\d[A-Za-z0-9+\-,.:\/<>]*$/.test(t))?'Not a POSIX timezone - it needs an offset, e.g. AEST-10AEDT,M10.1.0,M4.1.0/3, not Australia/Sydney. Pick one from the list above.':'');}
// What a bird's name will look like, from the settings above: the sizes as
// renderBirdPage sets them (a share of the page's short side, 1200 px; the
// scientific line a percentage of that; a gap of an eighth), drawn at half
// size in the frame's own two faces. The common name's face is capitals only,
// so a name not in capitals is set in the label face, as on the glass.
function namePreview(){var f=document.getElementById('settings'),g=function(n){return f.elements[n].value};
var px=1200*({0:.024,1:.032,2:.042,3:.055}[g('label')]||.032)*.5,how=g('names'),cs=g('namecase');
var common='Crimson Rosella',sci='Platycercus elegans';
common=cs=='1'?common.toUpperCase():cs=='2'?common.toLowerCase():common;
var a=document.getElementById('np1'),b=document.getElementById('np2'),box=document.getElementById('nameprev');
var first=how=='0'||how=='2'?common:how=='1'?sci:'',second=how=='0'?sci:'';
if(first&&f.elements.showconf[0].checked&&g('source')=='birdnet')first+=' (87%)';
a.textContent=first||'No names on the page';b.textContent=second;b.hidden=!second;
a.style.fontSize=(first?px:13)+'px';a.style.color=first?'':'var(--muted)';
var label=how=='1'||(first&&first!==first.toUpperCase());
a.style.fontFamily=!first?'system-ui,sans-serif':label?'BPLabel,Georgia,serif':'';a.style.fontStyle=label?'italic':'';
b.style.fontSize=Math.max(1,Math.round(px*(+g('scipct')/100)))+'px';b.style.marginTop=Math.round(px/8)+'px';
var cream=+g('cream');box.style.background='rgb('+[255-3*cream,255-5*cream,255-9*cream]+')'}
namePreview();
document.getElementById('settings').addEventListener('change',namePreview);
// The page's text bands, as renderBirdPage lays them out: the owner's lines
// and the note at the ends of their edges, at the name size as a
// share of the page; the values are the frame's own, as of loading this page.
var TV=%TEXTVALS%;
function textPreview(){var f=document.getElementById('settings'),el=f.elements,g=function(n){var e=el[n];return e?(e.length&&!e.tagName?e[0]:e).value:''};
var on=function(n){var e=el[n];e=e&&e.length&&!e.tagName?e[0]:e;return !!(e&&e.checked)};
var box=document.getElementById('textprev'),rot=+g('rotation'),portrait=(rot&1)==0,pw=portrait?1200:1600;
var w=box.clientWidth||300,k=w/pw;
var mg=g('marginmode')=='1'?[+g('margintop'),+g('marginright'),+g('marginbottom'),+g('marginleft')]:[+g('margin'),+g('margin'),+g('margin'),+g('margin')];
// Sizes as renderBirdPage sets them: a share of the page's short side
// (1200 px), never under kMinLabelPx. The frame's sizes are em sizes, as CSS
// font-size is, so a size times k is the size on the glass shrunk to the
// preview's width.
var sc={0:.024,1:.032,2:.042,3:.055},nameSc=sc[g('label')]||.032,size=function(n){return Math.max(11,Math.round(1200*n))};
var px=size(nameSc),inset=Math.max(4,px/2),linePx=function(v){v=+v;return size(v==5?.075:sc[v-1]||.032)};
// Only the sides' margins show: the strip is the two edges, not the page.
box.style.padding='0 '+mg[1]*k+'px 0 '+mg[3]*k+'px';
var src=g('source'),fresh=src=='birdnet'&&document.getElementById('tpnew').checked,vals={};
for(var n in TV)vals[n]=TV[n];
if(fresh){vals['new']=vals['new']||'Superb Fairywren';vals['new.count']=vals['new.count']||'1'}
// The times as the Times select says now, not as it was saved.
var clk=function(t){var m=/^(\d{1,2}):(\d\d)(?: ([ap])m)?$/.exec(t||'');if(!m)return t;var h=+m[1]%(m[3]?12:24)+(m[3]=='p'?12:0);
return g('clock')=='1'?((h%12||12)+':'+m[2]+(h<12?' am':' pm')):((h<10?'0':'')+h+':'+m[2])};
['time','next'].forEach(function(n){if(vals[n])vals[n]=clk(vals[n])});
var missing=[];
var fill=function(t){var gone=false,o=t.replace(/\{\{([^}]*)\}\}/g,function(m,n){n=n.replace(/ /g,'').toLowerCase();if(!(n in vals))return m;if(vals[n]===null){gone=true;missing.push(n);return ''}return vals[n]});return gone?'':o};
var swap=fresh&&on('newtext');
var top=swap&&g('newtoptext').trim()?g('newtoptext'):g('toptext'),bot=swap&&g('newbottomtext').trim()?g('newbottomtext'):g('bottomtext');
var bands=[[[],[],[]],[[],[],[]]],face=[g('topface')=='1',g('bottomface')=='1'];
var put=function(t,e,a,size,name){name=name&&face[e];if(t)bands[e][a].push({t:name?t.toUpperCase():t,px:size,name:name})};
var b=fill(bot.trim());
put(fill(top.trim()),0,+g('topalign'),linePx(g('topsize')),true);put(b,1,+g('bottomalign'),linePx(g('bottomsize')),true);
['tptop','tpbot'].forEach(function(id,e){var row=document.getElementById(id),any=false;
row.style.padding=(e?'0 ':inset*k+'px ')+inset*k+'px '+(e?inset*k+'px ':px/3*k+'px ')+inset*k+'px';
if(e)row.style.paddingTop=px/3*k+'px';
for(var a=0;a<3;a++){var span=row.children[a];span.innerHTML='';bands[e][a].forEach(function(r,i){any=true;var x=document.createElement('span');x.textContent=r.t;
x.style.fontSize=r.px*k+'px';x.style.fontFamily=r.name?'BPName,Georgia,serif':'BPLabel,Georgia,serif';x.style.fontStyle=r.name?'':'italic';if(i)x.style.marginLeft=px/2*k+'px';span.appendChild(x)})}
row.hidden=!any;row.style.fontSize='';
// Shrink to fit, as the frame does.
if(any&&row.scrollWidth>row.clientWidth)row.style.transform='scale('+row.clientWidth/row.scrollWidth+')',row.style.transformOrigin='left';else row.style.transform=''});
var cream=+g('cream');box.style.background='rgb('+[255-3*cream,255-5*cream,255-9*cream]+')';
var why={weather:'the weather comes with the next page (and needs a location)',next:'the next update needs the clock',battery:'this board cannot read its battery','battery.percent':'this board cannot read its battery',place:'this source has no place',radius:'this source has no place','new':'no bird is new today','new.count':'no bird is new today',birds:'no page fetched yet',top:'no page fetched yet','top.scientific':'no page fetched yet'};
var none=document.getElementById('tptop').hidden&&document.getElementById('tpbot').hidden;box.hidden=none;
document.getElementById('tpnote').textContent=none?'No text on the page.':missing.length?'A line is left off: '+(why[missing[0]]||(missing[0].indexOf('weather.')==0?why.weather:'the clock is not set yet'))+'.':''}
textPreview();
document.getElementById('settings').addEventListener('change',textPreview);
document.getElementById('settings').addEventListener('input',textPreview);
document.getElementById('settings').addEventListener('input',whereChanged);
// An example's Top or Bottom puts it in that line, as if typed there.
document.querySelectorAll('.examples button').forEach(function(b){b.onclick=function(){
var f=document.getElementById('settings').elements[b.dataset.to];f.value=b.closest('li').querySelector('code').textContent;
if(b.dataset.to.indexOf('new')==0){var n=document.getElementById('newtext');n.checked=true;newTextChanged()}
f.dispatchEvent(new Event('input',{bubbles:true}));f.focus()}});
window.addEventListener('resize',textPreview);
document.getElementById('settings').addEventListener('input',check);
document.getElementById('settings').addEventListener('change',check);
check();
var stamp=%STAMP%,sawBusy=false;
function watch(){fetch('/api/status',{cache:'no-store'}).then(function(r){return r.json()}).then(function(j){
if(j.phase){sawBusy=true;document.getElementById('phase').textContent=j.phase;document.getElementById('busy').hidden=false}
if(!j.phase&&(sawBusy||j.stamp!=stamp))location.reload();else setTimeout(watch,1000)}).catch(function(){setTimeout(watch,1500)})}
document.getElementById('refresh').addEventListener('submit',function(e){e.preventDefault();
document.getElementById('phase').textContent='starting';document.getElementById('busy').hidden=false;window.scrollTo(0,0);
fetch('/action',{method:'POST',body:new URLSearchParams({do:'refresh'})}).then(function(){setTimeout(watch,800)}).catch(function(){setTimeout(watch,800)});
setTimeout(function(){location.reload()},240000);});
if(!document.getElementById('busy').hidden)setTimeout(watch,1000);
</script>
</body></html>
)HTML";

// What the Test button answers: one small page, no tokens, so the form's
// values survive a tap on Back.
const char kTestPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bird poster - source test</title>
<style>
body{font:16px/1.5 system-ui,sans-serif;margin:0;padding:1rem;max-width:42rem;margin-inline:auto;background:#f4f1ea;color:#222}
h1{font:italic 1.8rem Georgia,serif;margin:.2rem 0 .6rem}
.status{background:#fff;border:1px solid #cfc9b8;border-radius:6px;padding:.8rem 1rem;font-size:.92rem}
.status div{display:flex;gap:.6rem}.status b{min-width:7rem;font-weight:600;color:#555}
.good{color:#2a5c2a}.bad{color:#b02020}
code{word-break:break-all;font-size:.85rem}
small{color:#666}
form{margin:1rem 0}
button{font:inherit;padding:.55rem 1rem;border-radius:6px;border:1px solid #2a5c2a;background:#2a5c2a;color:#fff;font-weight:600;cursor:pointer}
button:hover{background:#234d23}
</style></head><body>
<h1>Source test</h1>
)HTML";

String esc(const std::string &s) {
  String out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      // render() fills its %TOKEN%s one after another: a saved value that
      // held one would be filled in by a later pass.
      case '%': out += "&#37;"; break;
      default: out += c;
    }
  }
  return out;
}

const char *sel(bool on) { return on ? "selected" : ""; }

String fmt(double v) {
  char buf[24];
  snprintf(buf, sizeof buf, "%.5f", v);
  return buf;
}

std::string arg(const char *name) { return std::string(server.arg(name).c_str()); }

// Longest URL, key or timezone kept: NVS takes far more, but nothing real
// comes near it.
constexpr size_t kMaxText = 256;
// The page's own lines: more than fits across the glass at the smallest size.
constexpr size_t kMaxPageText = 120;

// A field typed or pasted: surrounding spaces and any control characters
// (a pasted newline, say) dropped. Not for the SSID or passwords, which may
// begin or end with a space on purpose.
std::string argTrim(const char *name) {
  std::string out;
  for (char c : arg(name))
    if (uint8_t(c) >= 0x20 && c != 0x7f) out += c;
  const size_t a = out.find_first_not_of(' ');
  if (a == std::string::npos) return "";
  return out.substr(a, out.find_last_not_of(' ') - a + 1);
}

// A whole number, clamped into range. Blank or not a number keeps what is
// saved: toInt() would read either as 0 and the clamp would make that the
// minimum.
int argInt(const char *name, int lo, int hi, int fallback) {
  if (!server.hasArg(name)) return fallback;
  const std::string text = argTrim(name);
  if (text.empty()) return fallback;
  char *end = nullptr;
  const long v = strtol(text.c_str(), &end, 10);
  if (*end != '\0') return fallback;
  return int(v < lo ? lo : (v > hi ? hi : v));
}

// "HH:MM" as a time input sends it, to minutes after midnight. Anything
// else keeps what is saved.
int argTime(const char *name, int fallback) {
  const std::string t = arg(name);
  int h = 0, m = 0;
  char tail = 0;
  if (sscanf(t.c_str(), "%d:%d%c", &h, &m, &tail) != 2) return fallback;
  if (h < 0 || h > 23 || m < 0 || m > 59) return fallback;
  return h * 60 + m;
}

String hhmm(int minutes) {
  minutes = (minutes % 1440 + 1440) % 1440;  // a day, whatever was stored
  char buf[8];
  snprintf(buf, sizeof buf, "%02d:%02d", minutes / 60, minutes % 60);
  return buf;
}

// For a string inside JSON: esc() is for HTML, and the page sets these with
// textContent, where an entity would show as typed.
String jsonEsc(const std::string &s) {
  String out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (uint8_t(c) < 0x20) {
      char buf[8];
      snprintf(buf, sizeof buf, "\\u%04x", c);
      out += buf;
    } else {
      out += c;
    }
  }
  return out;
}

bool validHostname(const std::string &h) {
  if (h.empty() || h.size() > 24 || h.front() == '-' || h.back() == '-') return false;
  for (char c : h)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  return true;
}

// While a page is being made the settings must not move under it, and a
// second draw queued behind the first would only cost another refresh.
bool refuseIfBusy(App &app) {
  if (app.phase.empty()) return false;
  server.send(503, "text/html",
              "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
              "<body style='font:16px system-ui;padding:1rem'><h2>Busy</h2><p>The frame is " +
                  esc(app.phase) + ". Try again in a minute.</p><p><a href='/'>Back</a></p></body>");
  return true;
}

// One token that changes when the frame does something the page should show.
String stamp(App &app) {
  return String((unsigned long)app.lastPresented) + "-" + String((unsigned long)app.state.lastRender) +
         "-" + esc(app.state.lastResult) + "-" + esc(app.lastKind);
}

// A coordinate as typed. Blank, letters or out of range all fail: atof would
// quietly read any of them as 0, which is a real place with no birds.
bool parseCoordinate(const char *name, double limit, double &out) {
  const std::string text = argTrim(name);
  if (text.empty()) return false;
  char *end = nullptr;
  const double v = strtod(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0') return false;
  // strtod reads "nan", and a NaN passes every comparison below.
  if (!std::isfinite(v) || v < -limit || v > limit) return false;
  out = v;
  return true;
}

// The source half of the form, as posted, over what is saved. `problem` is
// set when a field cannot be used, and the caller says so instead of saving.
Source sourceFromArg(const std::string &value, Source fallback) {
  if (value == "birdnet") return Source::BirdNet;
  if (value == "inat") return Source::iNaturalist;
  if (value == "ebird") return Source::eBird;
  if (value == "ala") return Source::Ala;
  if (value == "list") return Source::JsonList;
  return fallback;
}

// http(s)://, something after it, no spaces, and not absurdly long.
bool isHttpUrl(const std::string &url) {
  const size_t scheme = url.rfind("http://", 0) == 0 ? 7 : url.rfind("https://", 0) == 0 ? 8 : 0;
  return scheme && url.size() > scheme && url.size() <= kMaxText &&
         url.find(' ') == std::string::npos;
}

// eBird's keys are letters and digits: a dozen in the old ones, a UUID with
// its dashes in the new (which eBird refuses with the dashes taken out).
// Anything else is a paste gone wrong, and it goes into a request header,
// where a line break would not do.
bool validEbirdKey(const std::string &k) {
  if (k.empty() || k.size() > 64) return false;
  for (char c : k)
    if (!isalnum(uint8_t(c)) && c != '-') return false;
  return true;
}

bool validEbirdLocale(const std::string &l) {
  for (const char *ok : {"en_AU", "en", "en_NZ", "en_UK", "en_IN", "en_ZA"})
    if (l == ok) return true;
  return false;
}

// A POSIX TZ string's shape, loosely: a name of three letters or more (or
// <+10> in angle brackets), an offset, then only the characters the format
// uses. "Australia/Sydney" fails on the offset: newlib would read it as UTC,
// which would move the quiet hours without a word.
bool validTz(const std::string &tz) {
  if (tz.size() < 4 || tz.size() > 64) return false;
  size_t i = 0;
  if (tz[0] == '<') {
    i = tz.find('>');
    if (i == std::string::npos || i < 2) return false;
    ++i;
  } else {
    while (i < tz.size() && isalpha(uint8_t(tz[i]))) ++i;
    if (i < 3) return false;
  }
  if (i < tz.size() && (tz[i] == '+' || tz[i] == '-')) ++i;
  if (i >= tz.size() || !isdigit(uint8_t(tz[i]))) return false;
  for (char c : tz)
    if (!isalnum(uint8_t(c)) && !strchr("+-,.:/<>", c)) return false;
  return true;
}

SourceConfig sourceFromForm(App &app, std::string &problem) {
  SourceConfig cfg = app.sourceConfig();
  if (server.hasArg("source")) cfg.source = sourceFromArg(arg("source"), cfg.source);
  if (server.hasArg("detector")) cfg.detectorUrl = argTrim("detector");
  // The key is never sent back to the page, so a blank field means "keep the
  // saved one", as with the WiFi password.
  if (server.hasArg("ebirdkey") && !argTrim("ebirdkey").empty()) cfg.ebirdKey = argTrim("ebirdkey");
  if (server.hasArg("ebirdloc") && validEbirdLocale(arg("ebirdloc"))) cfg.ebirdLocale = arg("ebirdloc");
  if (server.hasArg("listurl")) cfg.listUrl = argTrim("listurl");
  cfg.minConfidence = argInt("bnconf", 0, 100, cfg.minConfidence);
  // In the unit the form was showing - which is the one it posts, even when
  // it has just been changed - and kept in kilometres.
  if (argInt("distunit", 0, 1, int(app.settings.miles)) == 1) {
    // Unchanged from what the page showed keeps the kilometres it came from.
    const int mi = argInt("radius", 1, 310, -1);
    if (mi > 0 && mi != kmToMiles(cfg.radiusKm)) cfg.radiusKm = std::clamp(milesToKm(mi), 1, 500);
  } else {
    cfg.radiusKm = argInt("radius", 1, 500, cfg.radiusKm);
  }
  cfg.inatVersion = argInt("inatv", 1, 2, app.settings.inatVersion);
  cfg.since = app.windowStart(argInt("lookback", 1, 10000, app.settings.lookback),
                              Settings::Lookback(argInt("lookbackunit", 0, 3, int(app.settings.lookbackUnit))));
  // The location only matters to the place-based sources; for the rest the
  // fields are hidden and whatever they hold is kept without being questioned.
  const bool latOk = parseCoordinate("lat", 90, cfg.lat);
  const bool lngOk = parseCoordinate("lng", 180, cfg.lng);
  const bool placeBased = cfg.source == Source::iNaturalist || cfg.source == Source::eBird ||
                          cfg.source == Source::Ala;
  if (placeBased) {
    if (!latOk) problem = "Latitude must be a number from -90 to 90.";
    else if (!lngOk) problem = "Longitude must be a number from -180 to 180.";
    else if (cfg.lat == 0 && cfg.lng == 0) problem = "Latitude and longitude are both 0 - that is the Gulf of Guinea. Use the place lookup or type the frame's location.";
    else if (cfg.source == Source::eBird && cfg.ebirdKey.empty()) problem = "eBird needs an API key - free from ebird.org/api/keygen.";
    else if (cfg.source == Source::eBird && !validEbirdKey(cfg.ebirdKey)) problem = "The eBird API key should be only letters, digits and dashes - check it was copied whole.";
    // Australia and its territories, generously: Christmas Island in the west,
    // Norfolk Island in the east, Macquarie Island in the south. ALA has
    // nothing outside it, and an empty page is a poor way to learn that.
    else if (cfg.source == Source::Ala && (cfg.lat < -56 || cfg.lat > -8 || cfg.lng < 104 || cfg.lng > 170))
      problem = "That place is outside Australia and its territories, which is all the Atlas of Living Australia covers - use iNaturalist or eBird there.";
  } else if (cfg.source == Source::BirdNet && !isHttpUrl(cfg.detectorUrl)) {
    problem = "The BirdNET-Go address must start with http:// or https://, with no spaces.";
  } else if (cfg.source == Source::JsonList && !isHttpUrl(cfg.listUrl)) {
    problem = "The list URL must start with http:// or https://, with no spaces.";
  }
  return cfg;
}

Mode modeFromForm(Source source, Mode fallback) {
  if (!server.hasArg("mode")) return fallback;
  const Mode mode = arg("mode") == "rarest" ? Mode::Rarest : Mode::MostDetected;
  // Rarest needs a global count, which BirdNET-Go does not have. The form
  // hides the option; a submit that carries it anyway lands on most-seen
  // rather than on a page that fails every hour.
  return supports(source, mode) ? mode : Mode::MostDetected;
}

// Every page-text name as the frame would fill it in now, for the settings
// page's preview: {"date.long":"26 September 2026", "battery":null, ...},
// null for one the frame has none of. '%' and '<' escaped, so neither the
// %TOKEN% passes nor the HTML parser can read anything into a value.
String textValues(App &app) {
  static const char *const kNames[] = {
      "date.long", "date.short", "date.numeric", "date.medium", "date.full", "time",
      "time.24h", "time.12h", "hour.24h", "hour.12h", "weekday", "weekday.short", "day", "month", "month.short", "month.number", "year",
      "next", "next.24h", "next.12h", "birds", "top", "top.scientific", "new", "new.count", "source", "window",
      "place", "radius", "refresh", "battery", "battery.percent", "weather.now", "weather.summary",
      "weather.today", "weather.high", "weather.low", "weather.rain", "weather.tomorrow",
      "weather.tomorrow.high", "weather.tomorrow.low", "weather.tomorrow.rain"};
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  if (now > 100000) localtime_r(&now, &tm);
  String json = "{";
  for (const char *name : kNames) {
    std::string out;
    const TextValue v = app.textValue(name, out);
    bool have = v == TextValue::Filled;
    if (v == TextValue::Unknown) {  // one of the clock's
      out = expandText(std::string("{{") + name + "}}", now > 100000 ? &tm : nullptr,
                       app.settings.textPrefs());
      have = !out.empty();
    }
    if (json.length() > 1) json += ',';
    json += "\"" + String(name) + "\":";
    if (!have) {
      json += "null";
      continue;
    }
    json += '"';
    for (char c : jsonEsc(out)) {
      if (c == '%') json += "\\u0025";
      else if (c == '<') json += "\\u003c";
      else json += c;
    }
    json += '"';
  }
  return json + "}";
}

String render(App &app, const std::string &error = "") {
  String page = FPSTR(kPage);
  const Settings &s = app.settings;
  const State &st = app.state;

  String net;
  if (WiFi.status() == WL_CONNECTED)
    net = "joined " + esc(s.wifiSsid) + " as " + WiFi.localIP().toString() + " (" +
          String(WiFi.RSSI()) + " dBm)";
  else if (WiFi.getMode() & WIFI_MODE_AP)
    net = "own network " + String(app.apSsid().c_str()) + " - not joined to a home network";
  else
    net = "off";
  page.replace("%NET%", net);
  const bool fetchOk = st.fetchOk;
  page.replace("%FETCHCLASS%", fetchOk ? "" : "bad");
  page.replace("%FETCH%", esc(fetchOk ? "ok, HTTP " + std::to_string(st.lastHttp)
                                      : (app.fetchError.empty() ? st.lastResult : app.fetchError)));
  page.replace("%GLASS%", esc(app.lastKind.empty() ? (st.showingStatus ? "status page" : "last page")
                                                    : app.lastKind));
  page.replace("%RENDERED%", esc(app.localTime(st.lastRender) +
                                 (st.lastBirds.empty() ? "" : " - " + st.lastBirds)));
  page.replace("%PLATES%",
               esc(app.platesOk ? std::to_string(app.plates.count()) + " species at " +
                                      std::to_string(app.plates.source()) + " px, " +
                                      (app.packOnCard ? "from the SD card" : "from flash") +
                                      (app.cardError.empty() || app.packOnCard ? "" : " (" + app.cardError + ")")
                                : app.platesError));
  page.replace("%VERSION%", kFirmwareVersion);
  // The artwork choice appears only where the filesystem holds more than one
  // pack; a 16 MB board has one and nothing to choose.
  {
    String packs;
    if (app.packs.size() > 1) {
      packs = "<div class=\"lh\"><label>Artwork</label><details class=\"info\"><summary title=\"More about this\">i</summary><div>The page draws from one region's plates. Changing it takes effect on the next page. A pack copied to the SD card as <code>plates-au.bin</code>, <code>plates-eu.bin</code> or <code>plates-us.bin</code> is used over the one in flash: the card holds the plates at full size.</div></details></div><select name=\"pack\">";
      for (const std::string &key : app.packs) {
        const char *label = key == "au"   ? "Australia - Gould's plates"
                            : key == "eu" ? "Europe - Gould's and the von Wrights' plates"
                            : key == "us" ? "North America - Audubon's plates"
                                          : key.c_str();
        const bool onCard =
            std::find(app.cardPacks.begin(), app.cardPacks.end(), key) != app.cardPacks.end();
        packs += "<option value=\"" + esc(key) + "\"" + (app.packKey == key ? " selected" : "") + ">" +
                 esc(label) + (onCard ? " (SD card, full size)" : "") + "</option>";
      }
      packs += "</select>";
    }
    page.replace("%PACKS%", packs);
  }
  {
    const std::time_t now = std::time(nullptr);
    String next;
    if (now < 100000) next = "no clock yet";
    else if (app.portalSleepAt)
      next = "WiFi off at " + String(app.clockTime(app.portalSleepAt).c_str()) +
             " unless this page is used; next page about " +
             String(app.clockTime(app.portalSleepAt + std::time_t(app.sleepSeconds(app.portalSleepAt))).c_str());
    else next = "next page about " + String(app.clockTime(now + std::time_t(app.sleepSeconds(now))).c_str());
    page.replace("%NEXT%", next);
  }
  page.replace("%PREVIEW%", app.lastKind.empty()
                                ? "<p><small>No page composed since waking. Actions below draw one.</small></p>"
                                : "<img class=\"preview\" src=\"/preview.bmp?v=" + String((unsigned long)app.lastPresented) +
                                      "\" alt=\"what the glass shows\">");
  page.replace("%ERROR%", error.empty() ? ""
                        : (error.rfind("Saved, except", 0) == 0 ? "<div class=\"err\">" : "<div class=\"busy\">") +
                              esc(error) + "</div>");
  // Inside the page's one script: a newline or backslash in the last result
  // (a service's own error text, a network's name) would end the string and
  // stop the whole script, every source's fields showing at once with it.
  page.replace("%STAMP%", "\"" + jsonEsc(stamp(app).c_str()) + "\"");
  page.replace("%BUSYSHOW%", app.phase.empty() ? "hidden" : "");
  {
    const bool online = WiFi.status() == WL_CONNECTED;
    page.replace("%ONLINE%", online ? "true" : "false");
    page.replace("%OFFLINE%", online ? "" : "hidden");
    page.replace("%OFFLINE_NOTE%",
                 online ? ""
                        : "<div class=\"busy\"><b>Not on a network yet.</b> Everything below can be saved now, "
                          "but the place lookup and the source test need the internet, which the frame's own "
                          "network does not have. Save the WiFi above first; once the frame has joined, open "
                          "it from your home network and finish here.</div>");
  }
  page.replace("%PHASE%", esc(app.phase));
  page.replace("%SSID%", esc(s.wifiSsid));
  {
    // Scanned on the first page served while unjoined, so the setup sheet
    // opens with the list already there. Later scans are by the button.
    if (!app.scanned && WiFi.status() != WL_CONNECTED && app.phase.empty()) app.scanNetworks();
    String options, list;
    for (const App::Network &n : app.networks) {
      const String name = esc(n.ssid);
      options += "<option value=\"" + name + "\"" + (n.ssid == s.wifiSsid ? " selected" : "") + ">" +
                 name + " (" + String(n.rssi) + " dBm" + (n.secure ? "" : ", open") + ")</option>";
      list += "<option value=\"" + name + "\">";
    }
    page.replace("%NETWORKS%", options);
    page.replace("%SSIDLIST%", list);
    page.replace("%SCANNOTE%", app.scanned ? (app.networks.empty() ? "none found - type the name below"
                                                                   : "pick one, or type the name below")
                                           : "not scanned yet");
  }
  page.replace("%PASSHINT%", s.wifiPass.empty() ? "none saved - open network" : "unchanged");
  page.replace("%HOST%", esc(s.hostname));
  page.replace("%SRC_INAT%", sel(s.source == Source::iNaturalist));
  page.replace("%SRC_BN%", sel(s.source == Source::BirdNet));
  page.replace("%SRC_EBIRD%", sel(s.source == Source::eBird));
  page.replace("%SRC_ALA%", sel(s.source == Source::Ala));
  page.replace("%SRC_LIST%", sel(s.source == Source::JsonList));
  // Never the key itself: only whether one is saved.
  page.replace("%EBIRDHINT%", s.ebirdKey.empty() ? "from ebird.org/api/keygen" : "saved - type a new key to replace it");
  page.replace("%EBIRDSAVED%", s.ebirdKey.empty() ? "0" : "1");
  page.replace("%EBL_AU%", sel(s.ebirdLocale == "en_AU"));
  page.replace("%EBL_EN%", sel(s.ebirdLocale == "en"));
  page.replace("%EBL_NZ%", sel(s.ebirdLocale == "en_NZ"));
  page.replace("%EBL_UK%", sel(s.ebirdLocale == "en_UK"));
  page.replace("%EBL_IN%", sel(s.ebirdLocale == "en_IN"));
  page.replace("%EBL_ZA%", sel(s.ebirdLocale == "en_ZA"));
  page.replace("%LISTURL%", esc(s.listUrl));
  page.replace("%MODE_MOST%", sel(s.mode == Mode::MostDetected));
  page.replace("%MODE_RARE%", sel(s.mode == Mode::Rarest));
  page.replace("%DETECTOR%", esc(s.detectorUrl));
  page.replace("%LAT%", fmt(s.lat));
  page.replace("%LNG%", fmt(s.lng));
  page.replace("%RADIUS%", String(s.miles ? kmToMiles(s.radiusKm) : s.radiusKm));
  page.replace("%RADIUSMAX%", s.miles ? "310" : "500");
  page.replace("%DISTNAME%", s.miles ? "miles" : "km");
  for (int v = 0; v < 2; ++v) page.replace("%DU" + String(v) + "%", sel(int(s.miles) == v));
  for (int v = 0; v < 2; ++v) page.replace("%TU" + String(v) + "%", sel(int(s.fahrenheit) == v));
  for (int v = 0; v < 2; ++v) page.replace("%CK" + String(v) + "%", sel(int(s.clock12h) == v));
  page.replace("%LOOKBACK%", String(s.lookback));
  page.replace("%INATV2%", sel(s.inatVersion == 2));
  page.replace("%INATV1%", sel(s.inatVersion == 1));
  for (int v = 0; v < 4; ++v) page.replace("%LB" + String(v) + "%", sel(int(s.lookbackUnit) == v));
  page.replace("%BIRDS%", String(s.birds));
  {
    // Stored in hours; shown in the largest unit it is a whole number of.
    const int unit = s.cycleHours > 0 && s.cycleHours % 168 == 0 ? 168
                     : s.cycleHours > 0 && s.cycleHours % 24 == 0 ? 24
                                                                  : 1;
    page.replace("%CYCLE%", String(s.cycleHours / unit));
    page.replace("%CYU1%", sel(unit == 1));
    page.replace("%CYU24%", sel(unit == 24));
    page.replace("%CYU168%", sel(unit == 168));
  }
  for (int r = 0; r < 4; ++r) page.replace("%ROT" + String(r) + "%", sel(s.rotation == r));
  for (int v = 0; v < 4; ++v) page.replace("%NAMES" + String(v) + "%", sel(int(s.names) == v));
  for (int v = 0; v < 3; ++v) page.replace("%CASE" + String(v) + "%", sel(int(s.commonCase) == v));
  for (int l = 0; l < 4; ++l) page.replace("%LBL" + String(l) + "%", sel(int(s.labelSize) == l));
  for (int v = 50; v <= 100; v += 10)
    page.replace("%SCI" + String(v) + "%", sel(s.sciPercent == v));
  for (int k = 0; k < 4; ++k) page.replace("%PKS" + String(k) + "%", sel(int(s.packStyle) == k));
  page.replace("%TOPTEXT%", esc(s.topText));
  page.replace("%CONFON%", s.showConfidence ? "checked" : "");
  page.replace("%TEXTVALS%", textValues(app));
  page.replace("%NEWTOP%", esc(s.newTopText));
  page.replace("%NEWBOT%", esc(s.newBottomText));
  page.replace("%NEWTEXTON%", s.newText ? "checked" : "");
  for (int v = 0; v < 2; ++v) page.replace("%TFC" + String(v) + "%", sel(int(s.topInNameFont) == v));
  for (int v = 0; v < 2; ++v) page.replace("%BFC" + String(v) + "%", sel(int(s.bottomInNameFont) == v));
  for (int v = 1; v < 6; ++v) page.replace("%TSZ" + String(v) + "%", sel(int(s.topSize) == v));
  for (int v = 1; v < 6; ++v) page.replace("%BSZ" + String(v) + "%", sel(int(s.bottomSize) == v));
  page.replace("%BOTTEXT%", esc(s.bottomText));
  for (int v = 0; v < 3; ++v) page.replace("%TAL" + String(v) + "%", sel(int(s.topAlign) == v));
  for (int v = 0; v < 3; ++v) page.replace("%BAL" + String(v) + "%", sel(int(s.bottomAlign) == v));
  page.replace("%EVERYON%", s.everyBird ? "checked" : "");
  page.replace("%SHUFON%", s.shuffleBirds ? "checked" : "");
  page.replace("%NEWON%", s.preferNew ? "checked" : "");
  page.replace("%BNCONF%", String(s.minConfidence));
  page.replace("%WEBON%", s.webPlates ? "checked" : "");
  page.replace("%WEBURL%", esc(s.webPlatesUrl));
  page.replace("%WEBLAST%", app.lastWebPlates.empty() ? String("")
                                                      : "Last page: " + esc(app.lastWebPlates) + ".");
  page.replace("%REFRESHCOUNT%",
               String((unsigned long)st.refreshes) + (st.refreshes == 1 ? " refresh" : " refreshes") +
                   (st.refreshesSince ? esc(" since " + app.localTime(st.refreshesSince)) : String("")));
  for (int v = 0; v < 2; ++v) page.replace("%DOR" + String(v) + "%", sel(int(s.dateOrder) == v));
  for (int v = 0; v < 5; ++v) page.replace("%VIV" + String(v) + "%", sel(s.vivid == v));
  for (int v = 0; v < 5; ++v) page.replace("%SHP" + String(v) + "%", sel(s.sharpen == v));
  for (int v = 0; v < 5; ++v) page.replace("%EDG" + String(v) + "%", sel(s.edges == v));
  for (int v = 0; v < 5; ++v) page.replace("%CRM" + String(v) + "%", sel(s.cream == v));
  for (int v = 0; v < 2; ++v) page.replace("%MGM" + String(v) + "%", sel(int(s.marginPerSide) == v));
  page.replace("%MARGIN%", String(s.margin));
  page.replace("%MARGINT%", String(s.marginTop));
  page.replace("%MARGINR%", String(s.marginRight));
  page.replace("%MARGINB%", String(s.marginBottom));
  page.replace("%MARGINL%", String(s.marginLeft));
  page.replace("%INTERVAL%", String(s.intervalMin));
  page.replace("%QFROM%", hhmm(s.quietFrom));
  page.replace("%QTO%", hhmm(s.quietTo));
  page.replace("%TZ%", esc(s.tz));
  return page;
}

// The last frame as a 4-bit BMP: no encoder to speak of, and every browser
// draws one. Palette is the panel's blended inks, so it looks like the glass.
void servePreview(App &app) {
  const Frame &f = app.last;
  if (app.lastKind.empty() || f.w == 0) {
    server.send(404, "text/plain", "nothing composed yet");
    return;
  }
  const uint32_t rowBytes = ((uint32_t(f.w) * 4 + 31) / 32) * 4;
  const uint32_t pixels = rowBytes * uint32_t(f.h);
  const uint32_t headers = 14 + 40 + 16 * 4;
  uint8_t hdr[14 + 40 + 64] = {0};
  auto put32 = [&](int at, uint32_t v) {
    hdr[at] = v & 255; hdr[at + 1] = (v >> 8) & 255; hdr[at + 2] = (v >> 16) & 255;
    hdr[at + 3] = (v >> 24) & 255;
  };
  hdr[0] = 'B'; hdr[1] = 'M';
  put32(2, headers + pixels);
  put32(10, headers);
  put32(14, 40);
  put32(18, uint32_t(f.w));
  put32(22, uint32_t(f.h));
  hdr[26] = 1; hdr[28] = 4;
  put32(34, pixels);
  put32(46, 16);
  put32(50, 16);
  for (int i = 0; i < 6; ++i) {
    hdr[54 + i * 4] = kInkRgb[i][2];
    hdr[55 + i * 4] = kInkRgb[i][1];
    hdr[56 + i * 4] = kInkRgb[i][0];
  }
  // The URL carries the render time, so this can be cached hard: a megabyte
  // over the radio each time the page is opened is the slowest thing here.
  server.sendHeader("Cache-Control", "max-age=31536000, immutable");
  server.setContentLength(headers + pixels);
  server.send(200, "image/bmp", "");
  NetworkClient client = server.client();
  client.write(hdr, headers);
  std::vector<uint8_t> row(rowBytes, 0);
  for (int y = f.h - 1; y >= 0; --y) {  // BMP rows run bottom-up
    const uint8_t *src = f.row(y);
    for (int x = 0; x < f.w; x += 2)
      row[x / 2] = uint8_t((src[x] << 4) | (x + 1 < f.w ? src[x + 1] : 0));
    if (client.write(row.data(), rowBytes) != rowBytes) break;
  }
}

void redirectHome() {
  server.sendHeader("Location", "/", true);
  server.send(303, "text/plain", "");
}

bool isOurHost(const String &host) {
  return host.startsWith(WiFi.softAPIP().toString()) ||
         host.startsWith(WiFi.localIP().toString()) || host.indexOf(".local") >= 0;
}

// Anything that is not for us gets sent to us: this is what turns the
// phone's connectivity check into the settings page.
void captiveRedirect() {
  server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

}  // namespace

void WebUi::begin(bool captive) {
  captive_ = captive;
  touched = false;
  pending_ = Request::None;

  server.on("/", HTTP_GET, [this]() {
    App &app = app_;
    touched = true;
    const std::string note = warning_;
    warning_.clear();
    server.send(200, "text/html", render(app, note));
  });
  // The frame's two faces, for the name preview on the settings page: the
  // same files it draws with. Cached by the browser for a day.
  for (const char *path : {kNameFontPath, kFontPath}) {
    server.on(String("/font") + path, HTTP_GET, [path]() {
      File f = LittleFS.open(path, "r");
      if (!f) {
        server.send(404, "text/plain", "no font");
        return;
      }
      server.sendHeader("Cache-Control", "max-age=86400");
      server.streamFile(f, "font/ttf");
      f.close();
    });
  }
  server.on("/preview.bmp", HTTP_GET, [this]() {
    touched = true;
    servePreview(app_);
  });
  server.on("/api/status", HTTP_GET, [this]() {
    App &app = app_;
    touched = true;
    // `stamp` changes whenever the frame's state moves on: the settings page
    // polls it after "fetch and draw" and reloads when it does.
    String json = "{\"stamp\":\"" + jsonEsc(stamp(app).c_str()) + "\",\"phase\":\"" + jsonEsc(app.phase) +
                  "\",\"ok\":" + (app.state.fetchOk ? "true" : "false") +
                  ",\"result\":\"" + jsonEsc(app.fetchError.empty() ? app.state.lastResult : app.fetchError) +
                  "\",\"glass\":\"" + jsonEsc(app.lastKind) + "\",\"lines\":[";
    bool first = true;
    for (const std::string &l : app.statusLines()) {
      if (!first) json += ",";
      first = false;
      json += "\"" + jsonEsc(l) + "\"";
    }
    json += "]}";
    server.send(200, "application/json", json);
  });
  // Where the last few wakes spent their time, newest first; see timing.h.
  server.on("/api/timing", HTTP_GET, [this]() {
    touched = true;
    server.send(200, "application/json", timing::json().c_str());
  });
#ifdef BIRDPOSTER_DEBUG
  // The memory stress test (App::stressTest): answered at once, run from the
  // portal's loop, watched through /api/timing's `pages` and /api/status's
  // phase. The glass is left alone throughout.
  server.on("/api/stress", HTTP_GET, [this]() {
    App &app = app_;
    touched = true;
    if (refuseIfBusy(app)) return;
    stressPages = std::clamp(atoi(arg("pages").c_str()), 1, 200);
    if (arg("pages").empty()) stressPages = 48;
    pending_ = Request::Stress;
    server.send(200, "application/json",
                ("{\"started\":" + std::to_string(stressPages) +
                 ",\"watch\":\"/api/timing (pages) and /api/status (phase)\"}").c_str());
  });
#endif
  // The chip's own speeds - memory, arithmetic, flash - measured on request;
  // a few seconds at full clock, so never on an ordinary wake. See bench.h.
  server.on("/api/bench", HTTP_GET, [this]() {
    App &app = app_;
    touched = true;
    if (refuseIfBusy(app)) return;
    const std::string json = bench::run(app.packOnCard ? std::string() : app.packPath);
    server.send(200, "application/json", json.c_str());
  });
  server.on("/wifi", HTTP_POST, [this]() {
    App &app = app_;
    touched = true;
    if (refuseIfBusy(app)) return;
    Settings &s = app.settings;
    std::string note;
    // The SSID and passwords are taken exactly as typed: spaces in them are
    // legal and may be meant.
    std::string newSsid = arg("ssid");
    if (newSsid.empty() || newSsid.size() > 32) {
      note += " The network name must be 1 to 32 characters.";
      newSsid = s.wifiSsid;
    }
    // Blank password fields mean "keep": they are never echoed into the page,
    // so a save with nothing typed must not wipe them. WPA2 wants 8 to 63;
    // anything else would only fail to join and drop back to setup.
    const std::string typedPass = arg("pass");
    std::string newPass = s.wifiPass;
    if (typedPass.size() >= 8 && typedPass.size() <= 63) newPass = typedPass;
    else if (!typedPass.empty()) note += " The WiFi password was not changed: it must be 8 to 63 characters.";
    const bool wifiChanged = newSsid != s.wifiSsid || newPass != s.wifiPass;
    s.wifiSsid = newSsid;
    s.wifiPass = newPass;
    saveSettings(s);
    if (wifiChanged) {
      app.state.wifiFailures = 0;
      // WiFi stays up after the restart, with the new address on the glass,
      // so the frame can be found on the network just joined.
      app.state.portalOn = true;
      saveState(app.state);
      server.send(200, "text/html",
                  "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
                  "<body style='font:16px system-ui;padding:1rem'><h2>Saved</h2>"
                  "<p>The frame will restart and join <b>" + esc(s.wifiSsid) +
                      "</b> and show its new address on the glass. If it cannot, it comes back on its own network with the setup page. "
                      "Once joined it answers at <a href='http://" + esc(s.hostname) +
                      ".local/'>http://" + esc(s.hostname) + ".local/</a>, where the bird "
                      "settings can reach iNaturalist.</p></body>");
      pending_ = Request::Reboot;
    } else {
      warning_ = note.empty() ? "WiFi settings saved." : "Saved, except:" + note;
      redirectHome();
    }
  });
  server.on("/frame", HTTP_POST, [this]() {
    App &app = app_;
    touched = true;
    if (refuseIfBusy(app)) return;
    Settings &s = app.settings;
    std::string note;
    const std::string host = arg("host");
    if (validHostname(host)) s.hostname = host;
    else if (!host.empty()) note += " The hostname was not changed: lower-case letters, digits and hyphens only.";
    // Blank means "keep", as for the WiFi password: it is never echoed back.
    const std::string ap = arg("appass");
    if (ap.size() >= 8 && ap.size() <= 63) s.apPass = ap;
    else if (!ap.empty()) note += " The setup network password was not changed: it must be 8 to 63 characters.";
    saveSettings(s);
    warning_ = note.empty() ? "Frame settings saved." : "Saved, except:" + note;
    redirectHome();
  });
  server.on("/save", HTTP_POST, [this]() {
    App &app = app_;
    touched = true;
    if (refuseIfBusy(app)) return;
    Settings &s = app.settings;
    // A bad location must not block the rest: everything else saves, the
    // location keeps what it had, and the next page says so.
    std::string problem, notes;
    const SourceConfig cfg = sourceFromForm(app, problem);
    s.source = cfg.source;
    s.mode = modeFromForm(cfg.source, s.mode);
    s.radiusKm = cfg.radiusKm;
    s.lookback = argInt("lookback", 1, 10000, s.lookback);
    s.lookbackUnit = Settings::Lookback(argInt("lookbackunit", 0, 3, int(s.lookbackUnit)));
    s.inatVersion = argInt("inatv", 1, 2, s.inatVersion);
    if (problem.empty()) {
      s.detectorUrl = cfg.detectorUrl;
      s.ebirdKey = cfg.ebirdKey;
      s.ebirdLocale = cfg.ebirdLocale;
      s.listUrl = cfg.listUrl;
      s.lat = cfg.lat;
      s.lng = cfg.lng;
    } else {
      notes += " The source's details: " + problem;
    }
    s.birds = argInt("birds", 1, 40, s.birds);
    s.everyBird = argInt("everybird", 0, 1, int(s.everyBird)) == 1;
    s.showConfidence = argInt("showconf", 0, 1, int(s.showConfidence)) == 1;
    s.rotation = argInt("rotation", 0, 3, s.rotation);
    s.names = NameStyle(argInt("names", 0, 3, int(s.names)));
    s.commonCase = NameCase(argInt("namecase", 0, 2, int(s.commonCase)));
    s.labelSize = LabelSize(argInt("label", 0, 3, int(s.labelSize)));
    s.sciPercent = argInt("scipct", 40, 100, s.sciPercent);
    s.packStyle = PackStyle(argInt("packstyle", 0, 3, int(s.packStyle)));
    // The checkbox comes before a hidden "0" of the same name, and the server
    // reads the first: "1" when ticked, the hidden "0" when not.
    s.shuffleBirds = argInt("shuffle", 0, 1, int(s.shuffleBirds)) == 1;
    s.preferNew = argInt("newfirst", 0, 1, int(s.preferNew)) == 1;
    s.minConfidence = argInt("bnconf", 0, 100, s.minConfidence);
    s.webPlates = argInt("webplates", 0, 1, int(s.webPlates)) == 1;
    if (server.hasArg("weburl")) {
      const std::string url = argTrim("weburl");
      if (isHttpUrl(url)) s.webPlatesUrl = url;
      else if (!url.empty()) notes += " The web plates address must start with http:// or https://, with no spaces.";
    }
    s.dateOrder = DateOrder(argInt("dateorder", 0, 1, int(s.dateOrder)));
    if (server.hasArg("toptext")) s.topText = argTrim("toptext").substr(0, kMaxPageText);
    if (server.hasArg("bottomtext")) s.bottomText = argTrim("bottomtext").substr(0, kMaxPageText);
    s.topAlign = TextAlign(argInt("topalign", 0, 2, int(s.topAlign)));
    if (server.hasArg("newtoptext")) s.newTopText = argTrim("newtoptext").substr(0, kMaxPageText);
    if (server.hasArg("newbottomtext")) s.newBottomText = argTrim("newbottomtext").substr(0, kMaxPageText);
    s.newText = argInt("newtext", 0, 1, int(s.newText)) == 1;
    s.miles = argInt("distunit", 0, 1, int(s.miles)) == 1;
    s.fahrenheit = argInt("tempunit", 0, 1, int(s.fahrenheit)) == 1;
    s.clock12h = argInt("clock", 0, 1, int(s.clock12h)) == 1;
    s.topInNameFont = argInt("topface", 0, 1, int(s.topInNameFont)) == 1;
    s.bottomInNameFont = argInt("bottomface", 0, 1, int(s.bottomInNameFont)) == 1;
    s.topSize = TextSize(argInt("topsize", 1, 5, int(s.topSize)));
    s.bottomSize = TextSize(argInt("bottomsize", 1, 5, int(s.bottomSize)));
    s.bottomAlign = TextAlign(argInt("bottomalign", 0, 2, int(s.bottomAlign)));
    s.vivid = argInt("vivid", 0, 4, s.vivid);
    s.sharpen = argInt("sharpen", 0, 4, s.sharpen);
    s.edges = argInt("edges", 0, 4, s.edges);
    s.cream = argInt("cream", 0, 4, s.cream);
    // Both sets of boxes are posted whichever is showing, so the one not in
    // use keeps its numbers for the next time it is picked.
    s.marginPerSide = argInt("marginmode", 0, 1, int(s.marginPerSide)) == 1;
    s.margin = argInt("margin", 0, 300, s.margin);
    s.marginTop = argInt("margintop", 0, 300, s.marginTop);
    s.marginRight = argInt("marginright", 0, 300, s.marginRight);
    s.marginBottom = argInt("marginbottom", 0, 300, s.marginBottom);
    s.marginLeft = argInt("marginleft", 0, 300, s.marginLeft);
    {
      // A count of hours, days or weeks; kept as hours, a year at most. A
      // form without the unit (an old page still open) meant hours.
      int unit = argInt("cycleunit", 1, 168, 1);
      if (unit != 24 && unit != 168) unit = 1;
      const int n = argInt("cycle", 0, 8760, -1);
      if (n >= 0) s.cycleHours = std::min(n * unit, 8760);
    }
    s.intervalMin = argInt("interval", 1, 1440, s.intervalMin);
    s.quietFrom = argTime("quietfrom", s.quietFrom);
    s.quietTo = argTime("quietto", s.quietTo);
    if (server.hasArg("pack") && arg("pack") != s.pack &&
        std::find(app.packs.begin(), app.packs.end(), arg("pack")) != app.packs.end()) {
      s.pack = arg("pack");
      app.openPack();  // the next page draws from it; the current one stays on the glass
    }
    const std::string tz = argTrim("tz");
    if (!tz.empty() && !validTz(tz)) {
      notes += " The timezone does not look like a POSIX one, e.g. AEST-10AEDT,M10.1.0,M4.1.0/3.";
    } else if (!tz.empty() && tz != s.tz) {
      s.tz = tz;
      setenv("TZ", tz.c_str(), 1);  // so the status card's times are right without a reboot
      tzset();
    }
    saveSettings(s);
    warning_ = notes.empty() ? "Settings saved." : "Saved, except:" + notes;
    redirectHome();
  });
  server.on("/test", HTTP_POST, [this]() {
    App &app = app_;
    touched = true;
    if (refuseIfBusy(app)) return;
    // The form as posted, not the saved settings: the point is to try values
    // before committing them. Missing fields fall back to what is saved.
    std::string problem;
    const SourceConfig cfg = sourceFromForm(app, problem);
    const Mode mode = modeFromForm(cfg.source, app.settings.mode);

    String page = FPSTR(kTestPage);
    bool saveable = false;
    if (!problem.empty()) {
      page += "<div class=\"status\"><div><b>Result</b><span class=\"bad\">" + esc(problem) + "</span></div></div>";
    } else if (WiFi.status() != WL_CONNECTED) {
      page += "<div class=\"status\"><div><b>Result</b><span class=\"bad\">not joined to a network - "
              "the frame cannot reach anything from its own setup network</span></div></div>";
    } else {
      const App::Probe r = app.probe(cfg, mode);
      const char *name = sourceName(cfg.source);
      page += "<div class=\"status\">";
      page += "<div><b>Source</b><span>" + String(name) + "</span></div>";
      page += "<div><b>Asked</b><span><code>" + esc(r.url) + "</code></span></div>";
      if (!r.ok) {
        page += "<div><b>Result</b><span class=\"bad\">" + esc(r.error) + "</span></div>";
        if (r.http == 0)
          page += "<div><b></b><span><small>" +
                  String(cfg.source == Source::BirdNet || cfg.source == Source::JsonList
                             ? "Nothing answered at that address. Check the host and port, and that the "
                               "frame and the server are on the same network."
                             : "The frame could not reach the service at all, so this is the network "
                               "rather than the settings: is this WiFi connected to the internet?") +
                  "</small></span></div>";
      } else {
        page += "<div><b>Result</b><span class=\"good\">connected, HTTP " + String(r.http) + "</span></div>";
        page += "<div><b>Species</b><span>" + String(r.seen) + " reported, " + String(r.drawable) +
                " with a plate" + (app.platesOk ? "" : " (no plate pack loaded)") + "</span></div>";
        if (!r.sample.empty()) {
          String names;
          for (const std::string &n : r.sample) names += (names.isEmpty() ? "" : ", ") + esc(n);
          page += "<div><b>For example</b><span>" + names + "</span></div>";
        }
        if (r.seen == 0)
          page += "<div><b></b><span class=\"bad\"><small>The source answered but reported nothing - " +
                  esc(emptyReplyHint(cfg.source, cfg)) + ".</small></span></div>";
        else if (r.drawable == 0 && app.platesOk)
          page += "<div><b></b><span class=\"bad\"><small>None of them has artwork in the pack, so the page would be empty.</small></span></div>";
      }
      page += "</div>";
      if (r.ok) {
        // The settings form as it was posted here, in order, so saving it is
        // the same as pressing Save on the settings page (checkboxes rely on
        // the order: the first of a name wins).
        page += "<form method=\"post\" action=\"/save\">";
        for (int i = 0; i < server.args(); i++) {
          if (server.argName(i) == "plain") continue;  // the WebServer's copy of the whole body
          page += "<input type=\"hidden\" name=\"" + esc(server.argName(i).c_str()) + "\" value=\"" +
                  esc(server.arg(i).c_str()) + "\">";
        }
        page += "<button type=\"submit\">Save settings</button></form>";
        saveable = true;
      }
    }
    page += "<p><a href=\"javascript:history.back()\">Back to settings</a> &middot; <a href=\"/\">Settings</a></p>";
    page += saveable ? "<p><small>Nothing is saved until you press Save settings.</small></p></body></html>"
                     : "<p><small>Nothing was saved. The Save button on the settings page does that.</small></p></body></html>";
    server.send(200, "text/html", page);
  });
  server.on("/action", HTTP_POST, [this]() {
    App &app = app_;
    touched = true;
    if (refuseIfBusy(app)) return;
    const std::string what = arg("do");
    if (what == "refresh") {
      // Long: fetch, pack, dither and a 30 s refresh. Answer first so the
      // browser is not left hanging on a request that then times out.
      redirectHome();
      pending_ = Request::Refresh;
    } else if (what == "status") {
      redirectHome();
      app.state.showingStatus = true;
      saveState(app.state);
      app.showStatus();
    } else if (what == "scan") {
      app.scanNetworks();
      redirectHome();
    } else if (what == "pattern") {
      redirectHome();
      app.showPattern();
    } else if (what == "setup") {
      redirectHome();
      app.showSetup(app.apSsid(), app.settings.apPass, "http://192.168.4.1/");
    } else if (what == "sleep") {
      server.send(200, "text/html", "<meta charset=utf-8><p style='font:16px system-ui;padding:1rem'>Going back to sleep. Press key 1 to wake the settings again.</p>");
      pending_ = Request::Sleep;
    } else if (what == "resetcount") {
      app.resetRefreshCount();
      warning_ = "Refresh count cleared to 0.";
      redirectHome();
    } else if (what == "reboot") {
      server.send(200, "text/html", "<meta charset=utf-8><p style='font:16px system-ui;padding:1rem'>Rebooting.</p>");
      pending_ = Request::Reboot;
    } else if (what == "forget") {
      app.settings.wifiSsid.clear();
      app.settings.wifiPass.clear();
      saveSettings(app.settings);
      server.send(200, "text/html", "<meta charset=utf-8><p style='font:16px system-ui;padding:1rem'>Forgotten. The frame will restart on its own network.</p>");
      pending_ = Request::Reboot;
    } else {
      redirectHome();
    }
  });

  // Connectivity probes. Each OS has its own, and every one of them wants
  // something other than what it asks for to decide it is behind a portal.
  for (const char *probe : {"/generate_204", "/gen_204", "/hotspot-detect.html", "/connecttest.txt",
                            "/ncsi.txt", "/redirect", "/canonical.html", "/success.txt",
                            "/library/test/success.html", "/check_network_status.txt"})
    server.on(probe, [this]() {
      touched = true;
      if (captive_) captiveRedirect();
      else server.send(404, "text/plain", "");
    });
  server.onNotFound([this]() {
    touched = true;
    if (captive_ && !isOurHost(server.hostHeader())) captiveRedirect();
    else server.send(404, "text/plain", "not here");
  });

  if (captive) dns.start(53, "*", WiFi.softAPIP());
  server.begin();
  active_ = true;
}

void WebUi::stop() {
  if (!active_) return;
  server.stop();
  if (captive_) dns.stop();
  active_ = false;
}

bool WebUi::poll() {
  if (!active_ || inHandler_) return false;
  if (captive_) dns.processNextRequest();
  inHandler_ = true;
  server.handleClient();
  inHandler_ = false;
  const bool was = touched;
  touched = false;
  return was;
}

WebUi::Request WebUi::take() {
  const Request r = pending_;
  pending_ = Request::None;
  return r;
}

}  // namespace birdposter
