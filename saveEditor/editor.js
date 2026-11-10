'use strict';
(() => {
const $=id=>document.getElementById(id);
const plants=['豌豆射手','向日葵','樱桃炸弹','坚果','土豆雷','寒冰射手','大嘴花','双发射手','小喷菇','阳光菇','大喷菇','墓碑吞噬者','魅惑菇','胆小菇','寒冰菇','毁灭菇','睡莲','窝瓜','三线射手','缠绕海草','火爆辣椒','地刺','火炬树桩','高坚果','海蘑菇','路灯花','仙人掌','三叶草','裂荚射手','杨桃','南瓜头','磁力菇','卷心菜投手','花盆','玉米投手','咖啡豆','大蒜','叶子保护伞','金盏花','西瓜投手','机枪射手','双子向日葵','忧郁菇','香蒲','冰西瓜','吸金磁','地刺王','玉米加农炮'];
let model={scene:2,rake:0,cells:Array(54).fill(0)},presets=[],custom=[],past=[],future=[],busy=false,eraser=false,host=false,initialized=false,sequence=0;
let library,standalone;
const pending=new Map(),channel='pvz-save-editor-v1',origin=new URL(document.baseURI).origin;
const rows=()=>model.scene===2||model.scene===3?6:5;
function request(op,payload={}){
 if(standalone)return standalone.request(op,payload);
 if(!host)return Promise.reject(Error('DAT64 操作需从 pvz_perf 的“自定义存档”入口打开；独立页面可编辑阵型码及布阵 JSON。'));
 const id=++sequence;return new Promise((resolve,reject)=>{const timer=setTimeout(()=>{pending.delete(id);reject(Error('存档操作超时，请勿刷新；先确认本地存档状态。'))},240000);pending.set(id,{resolve,reject,timer});parent.postMessage({channel,id,op,payload},origin)});
}
window.addEventListener('message',event=>{
 if(event.source!==parent||event.origin!==origin||event.data?.channel!==channel)return;
 const data=event.data;
 if(data.op==='init'){
  host=true;document.documentElement.toggleAttribute('data-game-host',data.payload.gameHost===true);document.querySelector('main').inert=false;document.querySelector('nav').inert=false;loadProfiles(data.payload.profiles);presets=data.payload.presets;custom=data.payload.custom||[];fillSaves(data.payload.saves);if(!initialized){setModel(data.payload.lineup);$('code').value=data.payload.code;initialized=true}fillPresets();tab('profile');status(data.payload.gameHost?'已打开。存档导入、导出与修复在游戏启动页的“高级”中。':'已打开。修改游戏存档前可先导出 DAT64 备份。');return;
 }
 const item=pending.get(data.id);if(!item)return;pending.delete(data.id);clearTimeout(item.timer);data.error?item.reject(Error(data.error)):item.resolve(data.result);
});
function status(text,error=false){$('status').textContent=text;$('status').classList.toggle('error',error)}
function confirmAction(message){
 const dialog=$('editor-confirm');$('editor-confirm-message').textContent=message;
 // Toy's ancestor iframe may forbid native modal dialogs. An inner iframe
 // cannot restore allow-modals, so use an ordinary HTML dialog throughout.
 for(const button of dialog.querySelectorAll('button')){button.disabled=false;button.onclick=()=>dialog.close(button.value)}
 dialog.returnValue='cancel';
 return new Promise(resolve=>{dialog.addEventListener('close',()=>{const confirmed=dialog.returnValue==='confirm';if(!confirmed)status('已取消操作。');resolve(confirmed)},{once:true});dialog.showModal()});
}
async function action(fn){if(busy)return;busy=true;const disabled=[...document.querySelectorAll('button,input,select,textarea')].map(el=>[el,el.disabled]);disabled.forEach(([el])=>el.disabled=true);try{status('处理中…');await fn()}catch(error){status(error.message||String(error),true)}finally{busy=false;disabled.forEach(([el,value])=>el.disabled=value);refreshProfileControls();refreshLineupControls();describeSlot()}}
function tab(name){document.querySelectorAll('.tab').forEach(el=>el.hidden=el.id!==name);document.querySelectorAll('[data-tab]').forEach(el=>el.setAttribute('aria-selected',String(el.dataset.tab===name)))}
document.querySelectorAll('[data-tab]').forEach(el=>el.onclick=()=>tab(el.dataset.tab));
function refreshLineupControls(){$('delete-lineup').disabled=busy||![...presets,...custom].some(p=>p.code===$('preset').value)}
function syncPresetSelection(code=$('code').value){$('preset').value=code;if($('preset').selectedIndex<0)$('preset').value='';refreshLineupControls()}
function fillPresets(code=$('code').value){const draft=new Option('当前布阵（未保存到列表）','');draft.disabled=true;$('preset').replaceChildren(draft,...[...presets,...custom].map(p=>new Option(p.name,p.code)));syncPresetSelection(code)}
function fillSaves(names){const selected=$('save-list').value;$('save-list').replaceChildren(...names.map(name=>new Option(name,name)));if(names.includes(selected))$('save-list').value=selected;else if(names.includes('game1_58.dat64'))$('save-list').value='game1_58.dat64'}
function checkpoint(){past.push(structuredClone(model));if(past.length>100)past.shift();future=[]}
function setModel(next){++revision;model={scene:Number(next.scene),rake:Number(next.rake||0),cells:Array.from(next.cells)};if(model.cells.length!==54)throw Error('阵型必须有 54 格');$('scene').selectedIndex=model.scene;render()}
function paintOptions(){const layer=$('layer').value;let choices=layer==='main'?plants.map((name,i)=>[i,name]).filter(([i])=>![16,30,33,35].includes(i)):layer==='base'?[[0,'无'],[1,'睡莲'],[2,'花盆'],[3,'墓碑']]:[[1,layer==='pumpkin'?'南瓜头':layer==='coffee'?'咖啡豆':'梯子']];$('plant').replaceChildren(...choices.map(([value,label])=>new Option(label,value)));$('awake').disabled=layer!=='main';$('imitater').disabled=layer==='ladder'||layer==='base'&&$('plant').value==='3'}
function cellLabel(c){const type=(c>>>10)-1,base=(c>>>6)&3;return {main:type>=0?plants[type]+(c&512?'·仿':'')+(c&256?'':'·眠'):'',base:['','睡莲','花盆','墓碑'][base],extras:[c&16?'南瓜':'',c&4?'咖啡':'',c&1?'梯子':''].filter(Boolean).join(' ')} }
function render(){const board=$('board');board.replaceChildren();for(let i=0;i<rows()*9;i++){const c=model.cells[i],label=cellLabel(c),button=document.createElement('button');button.type='button';button.setAttribute('role','gridcell');button.dataset.cell=i;button.classList.toggle('water',(model.scene===2||model.scene===3)&&i>=18&&i<36);button.classList.toggle('roof',model.scene>=4);button.classList.toggle('night',[1,3,5].includes(model.scene));button.setAttribute('aria-label',`${Math.floor(i/9)+1}排${i%9+1}列 ${Object.values(label).join(' ')||'空'}`);for(const name of ['base','main','extras']){const span=document.createElement('span');span.className=name;span.textContent=label[name]||'\u00a0';button.append(span)}button.onclick=event=>edit(i,eraser||event.shiftKey);button.oncontextmenu=event=>{event.preventDefault();edit(i,true)};board.append(button)}$('tile-info').textContent=`${rows()} 排 × 9 列；${model.cells.filter(c=>c>>>10).length} 株主植物。阵型不保存血量；底座和附加物独立编辑。`}
function edit(i,remove){if(busy)return;checkpoint();let c=model.cells[i],value=Number($('plant').value),im=$('imitater').checked?1:0;switch($('layer').value){case'main':c=(c&255)|(remove?0:((value+1)<<10)|(im<<9)|($('awake').checked?256:0));break;case'base':c=(c&~224)|(remove?0:(value<<6)|(value===3?0:im<<5));break;case'pumpkin':c=(c&~24)|(remove?0:16|(im<<3));break;case'coffee':c=(c&~6)|(remove?0:4|(im<<1));break;case'ladder':c=(c&~1)|(remove?0:1);break;}model.cells[i]=c;render();updateCode().catch(error=>status(error.message,true))}
async function decode(code){
 code=code.trim().replace(/\s+/g,'').replace(/-/g,'+').replace(/_/g,'/');if(!/^[A-Za-z0-9+/]{16,164}={0,2}$/.test(code)||code.length%4)throw Error('阵型码格式错误');
 const bytes=Uint8Array.from(atob(code),c=>c.charCodeAt(0)^84),meta=bytes.at(-1),scene=meta&15,rake=meta>>>4,rows=scene===2||scene===3?6:5;if(scene>5||rake>rows||([2,3].includes(scene)&&[3,4].includes(rake)))throw Error('阵型的场景或钉耙位置无效');
 const reader=new Blob([bytes.slice(0,-1)]).stream().pipeThrough(new DecompressionStream('deflate')).getReader(),chunks=[];let length=0;try{for(;;){const {value,done}=await reader.read();if(done)break;length+=value.length;if(length>108)throw Error('阵型数据过长');chunks.push(value)}}finally{await reader.cancel().catch(()=>{})}
 if(length!==rows*18)throw Error('阵型格数不匹配');const raw=new Uint8Array(length);let at=0;for(const part of chunks){raw.set(part,at);at+=part.length}const view=new DataView(raw.buffer),cells=Array.from({length:54},(_,i)=>i<rows*9?view.getUint16(i*2,true):0);if(cells.some(c=>(c>>>10)>48))throw Error('植物类型无效');return {scene,rake,cells};
}
let revision=0;
async function updateCode(){const current=++revision,snapshot=structuredClone(model),raw=new Uint8Array((snapshot.scene===2||snapshot.scene===3?6:5)*18),view=new DataView(raw.buffer);snapshot.cells.slice(0,raw.length/2).forEach((c,i)=>view.setUint16(i*2,c,true));const packed=new Uint8Array(await new Response(new Blob([raw]).stream().pipeThrough(new CompressionStream('deflate'))).arrayBuffer()),bytes=new Uint8Array(packed.length+1);bytes.set(packed);bytes[packed.length]=(snapshot.rake<<4)|snapshot.scene;if(current===revision){$('code').value=btoa(String.fromCharCode(...bytes.map(x=>x^84)));syncPresetSelection()}}
$('layer').onchange=paintOptions;$('plant').onchange=()=>{$('imitater').disabled=$('layer').value==='base'&&$('plant').value==='3'};
$('erase').onclick=()=>{eraser=!eraser;$('erase').setAttribute('aria-pressed',String(eraser))};
$('undo').onclick=()=>{if(past.length){future.push(structuredClone(model));setModel(past.pop());updateCode()}};$('redo').onclick=()=>{if(future.length){past.push(structuredClone(model));setModel(future.pop());updateCode()}};
$('scene').onchange=()=>action(async()=>{const next=$('scene').selectedIndex;if(![2,3].includes(next)&&model.cells.slice(45).some(Boolean)){$('scene').selectedIndex=model.scene;throw Error('第六排有物体，请清空后再换成五排场景')}checkpoint();model.scene=next;render();await updateCode();status('场景已切换。')});
$('load-code').onclick=()=>action(async()=>{const next=await decode($('code').value);checkpoint();setModel(next);syncPresetSelection();status('已读取阵型码。')});
$('preset').onchange=()=>action(async()=>{const p=[...presets,...custom].find(p=>p.code===$('preset').value);if(!p)throw Error('没有可载入的阵型');const next=await decode(p.code);checkpoint();setModel(next);$('code').value=p.code;status('已载入 '+p.name)});
$('add-lineup').onclick=()=>action(async()=>{await updateCode();const entry={name:$('lineup-name').value.trim()||'我的阵型',code:$('code').value};if(host){const result=await request('save-lineup',entry);custom=result.custom;if(result.presets)presets=result.presets}else{library.add(entry);custom=library.snapshot().lineups}fillPresets(entry.code);status('阵型已保存。')});
function recipe(){const flags=Number($('flags').value),seed=Number($('seed').value),sun=Number($('sun').value);if(!Number.isInteger(flags)||flags<0||flags>10000000||flags%2)throw Error('旗数需为 0～10000000 的偶数');if(!Number.isInteger(seed)||seed<-2147483648||seed>2147483647)throw Error('种子需为 32 位整数');if(!Number.isInteger(sun)||sun<0||sun>9990)throw Error('阳光需为 0～9990 的整数');return {...structuredClone(model),flags,seed,sun}}
function download(data,name,type){const url=URL.createObjectURL(new Blob([data],{type})),a=document.createElement('a');a.href=url;a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(url),60000)}
$('recipe').onclick=()=>action(async()=>{download(JSON.stringify(recipe(),null,2),'pvz-lineup.json','application/json');status('已导出布阵 JSON。')});
$('generate').onclick=()=>action(async()=>{const name=$('generate-filename').value.trim(),result=await request('generate',{name,recipe:recipe()});fillSaves(result.saves);if(result.profiles)loadProfiles(result.profiles);status('已生成并部署 '+name+'。')});
$('extract').onclick=()=>action(async()=>{const result=await request('extract',{name:$('save-list').value});checkpoint();setModel(result.metadata);$('sun').value=result.metadata.sun;$('flags').value=result.metadata.flags;$('seed').value=result.metadata.seed;$('generate-filename').value=$('save-list').value;describeSlot();await updateCode();tab('lineup');status('已提取阵型；修改后生成新档会恢复植物血量。')});
$('export').onclick=()=>action(async()=>{const name=$('save-list').value,result=await request('export',{name});download(result.bytes,name,'application/octet-stream');status('已导出 '+name)});
$('file').onchange=()=>{const file=$('file').files[0];if(/^game[1-9]\d*_\d+\.dat64$/.test(file?.name))$('filename').value=file.name};
$('import').onclick=()=>action(async()=>{const file=$('file').files[0];if(!file)throw Error('请选择 DAT64');if(file.size>32*1024*1024)throw Error('DAT64 超过 32 MiB');const result=await request('import',{name:$('filename').value.trim(),bytes:new Uint8Array(await file.arrayBuffer())});fillSaves(result.saves);status('已校验并导入，关卡仅保存在本地。')});
$('repair-plants').onclick=()=>action(async()=>{const name=$('save-list').value,result=await request('repair-plant-attacks',{name});if(result.bytes)download(result.bytes,name.replace('.dat64','.repaired.dat64'),'application/octet-stream');status(result.bytes?'已下载修复副本，原档未改动。':'没有发现已知植物卡死状态，原档未改动。')});
$('export-users').onclick=()=>action(async()=>{const result=await request('export-users');for(const [name,bytes]of result.files)download(bytes,name,'application/octet-stream');status('已导出用户列表和用户进度；关卡 DAT64 请单独导出。')});
$('close').onclick=()=>{if(host)request('close').catch(error=>status(error.message,true));else status('可直接关闭此页面。')};
paintOptions();render();if(parent!==window){document.querySelector('main').inert=true;document.querySelector('nav').inert=true;parent.postMessage({channel,op:'ready'},origin)}else{
 document.querySelector('main').inert=true;document.querySelector('nav').inert=true;
 import('./save-standalone.js').then(async({createStandalone})=>{standalone=await createStandalone();window.postMessage({channel,op:'init',payload:await standalone.initial()},origin)}).catch(error=>status('独立工具加载失败：'+error.message,true));
}
})();
