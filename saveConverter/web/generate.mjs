import {encodeModel,uint,int,float,putInt,putFloat,putDouble,time} from './model.mjs';
import {decodeCatalog,appendPrefab} from './prefab.mjs';
const initialRecordsGzip = /* INITIAL_RECORD_DATA */ null;
let catalogPromise;
function integer(n,min,max,label){if(!Number.isInteger(n)||n<min||n>max)throw Error(label+'超出允许范围');return n}
async function catalog(){
 return catalogPromise??=(async()=>{
  if(typeof initialRecordsGzip!=='string')throw Error('请使用已构建的独立存档工具');
  const bytes=Uint8Array.from(atob(initialRecordsGzip),c=>c.charCodeAt(0));
  const text=await new Response(new Blob([bytes]).stream().pipeThrough(new DecompressionStream('gzip'))).text(),data=decodeCatalog(text);
  if(data.version!==1)throw Error('存档初始记录版本不兼容');return data;
 })().catch(error=>{catalogPromise=null;throw Error('存档初始记录解码失败：'+(error.message||error))});
}
const gridX=x=>40+80*x;
const gridY=(scene,x,y)=>80+(scene>=4?85*y+Math.max(0,5-x)*20-10:scene===2||scene===3?85*y:100*y);
function movePlant(prefab,x,y,scene,im){
 const plant=prefab.pools[1][0],r=plant.r,v=plant.v,oldX=float(v,0),oldY=float(v,4),oldCol=int(r,52),oldRow=int(r,36);
 const dx=gridX(x)-gridX(oldCol),dy=gridY(scene,x,y)-gridY(scene,oldCol,oldRow),order=(y-oldRow)*10000-dx;
 putInt(r,16,Math.trunc(oldX+dx));putInt(r,20,Math.trunc(oldY+dy));putInt(r,36,y);putInt(r,40,int(r,40)+order);putInt(r,52,x);putInt(r,148,y);putInt(r,324,im?48:-1);
 putFloat(v,0,oldX+dx);putFloat(v,4,oldY+dy);putDouble(v,8,oldX+dx);
 const sleeping=uint(r,184);
 for(const animation of prefab.pools[9]){putInt(animation.r,124,int(animation.r,124)+order);if(animation.id===sleeping){putFloat(animation.r,52,float(animation.r,52)+dx);putFloat(animation.r,64,float(animation.r,64)+dy)}}
 for(const system of prefab.pools[6])putInt(system.r,60,int(system.r,60)+order);
 for(const emitter of prefab.pools[7]){putFloat(emitter.r,52,float(emitter.r,52)+dx);putFloat(emitter.r,56,float(emitter.r,56)+dy)}
 for(const particle of prefab.pools[8]){putFloat(particle.r,36,float(particle.r,36)+dx);putFloat(particle.r,40,float(particle.r,40)+dy)}
}
function moveGrid(prefab,x,y,scene){
 const item=prefab.pools[5][0],r=item.r,oldX=int(r,24),oldY=int(r,28),dx=gridX(x)-gridX(oldX),dy=gridY(scene,x,y)-gridY(scene,oldX,oldY),order=(y-oldY)*10000;
 putInt(r,24,x);putInt(r,28,y);putInt(r,36,int(r,36)+order);putFloat(r,44,float(r,44)+dx);putFloat(r,48,float(r,48)+dy);
 for(const animation of prefab.pools[9]){putFloat(animation.r,52,float(animation.r,52)+dx);putFloat(animation.r,64,float(animation.r,64)+dy);putInt(animation.r,124,int(animation.r,124)+order)}
}
const musicStarts=[[115,3],[115,3],[120,3],[71,2],[183,5],[80,2],[100,3],[183,5],[80,2],[100,3],[160,3],[80,2],[80,2],[115,3]];
const musicOrders=[0,0,0x30,0x5e,0x7d,0xb8,0x7a,0x98,0xdd,0xb1,0xa6,0xd4,0x9e,0];
export async function generateSave(core,request,profile){
 const mode=integer(request.mode??58,0,72,'关卡模式');if([34,35,71].includes(mode))throw Error('此模式没有关卡存档');
 if(!(profile instanceof Uint8Array)||profile.length<894||new TextDecoder().decode(profile.slice(0,8))!=='PVZUSR64')throw Error('用户明细无效');
 const profileView=new DataView(profile.buffer,profile.byteOffset,profile.byteLength);if(profileView.getInt32(8,true)!==13)throw Error('用户明细版本无效');
 const level=mode===0?integer(profileView.getInt32(12,true),1,50,'冒险关卡'):1;
 const recipe=request.recipe;if(!recipe)throw Error('缺少布阵参数');
 const scene=integer(recipe.scene,0,5,'场景'),rows=scene===2||scene===3?6:5;
 const flags=integer(recipe.flags,0,10000000,'已完成旗数');if(flags%2)throw Error('已完成旗数必须为偶数');
 const seed=integer(recipe.seed,-2147483648,2147483647,'随机数种子'),sun=integer(recipe.sun??9990,0,9990,'阳光'),rake=integer(recipe.rake,0,rows,'耙子行');
 if((scene===2||scene===3)&&(rake===3||rake===4))throw Error('水路不能放置耙子');
 if(!Array.isArray(recipe.cells)||recipe.cells.length!==54)throw Error('布阵必须包含 54 格');
 const cells=recipe.cells.map((cell,at)=>{integer(cell,0,65535,'布阵编码');if(cell>>>10>48||(at>=rows*9&&cell))throw Error('布阵植物或第六行无效');return cell});
 const data=await catalog(),schema=data.schema,doc=structuredClone(data.bases[mode===0&&profileView.getInt32(20,true)===0?'first:'+level:mode+':'+level]);if(!doc)throw Error('缺少此关卡的初始化记录');
 const environment=data.bases['scene'+scene];
 doc.board.r.set(environment.board.r.subarray(4,220),4);doc.board.r.set(environment.board.r.subarray(1140,1164),1140);
 putInt(doc.board.r,22008-528,scene);putInt(doc.board.r,22028-528,sun);putInt(doc.board.r,22216-528,seed);doc.board.r[22173-528]=1;
 time(doc.board,schema.fixed[0],'mFogBlownCountDown',0n);putFloat(doc.board.r,1660-528,0);
 const endless=mode>=56&&mode<=60;
 if(endless){putInt(doc.fixed[4].r,116,flags/2-1);const waves=int(doc.board.r,22032-528);putInt(doc.board.r,22056-528,waves);putInt(doc.board.r,22060-528,waves);time(doc.board,schema.fixed[0],'mNextSurvivalStageCounter',10000000n);putInt(doc.fixed[3].r,48,0)}
 const add=(at,type,im,asleep=false)=>{const original=data.plants[scene+':'+type+':'+Number(asleep)];if(!original)throw Error('缺少植物初始记录');const prefab=appendPrefab(doc,original,schema);movePlant(prefab,at%9,Math.floor(at/9),scene,im);return prefab};
 for(let at=0;at<rows*9;++at){const cell=cells[at],base=cell>>6&3;if(base===1||base===2)add(at,base===1?16:33,!!(cell&32))}
 for(let at=0;at<rows*9;++at){const cell=cells[at],type=(cell>>10)-1;if(type>=0)add(at,type,!!(cell&512),!(cell&256))}
 for(let at=0;at<rows*9;++at){const cell=cells[at];if(cell&16)add(at,30,!!(cell&8));if(cell&4)add(at,35,!!(cell&2));for(const [kind,enabled]of [['grave',(cell>>6&3)===3],['ladder',!!(cell&1)]])if(enabled)moveGrid(appendPrefab(doc,data.grids[scene+':'+kind],schema),at%9,Math.floor(at/9),scene)}
 if(rake)moveGrid(appendPrefab(doc,data.grids[scene+':rake'],schema),7,rake-1,scene);
 const music=doc.fixed[5],previous=int(music.r,16),tune=endless?6:previous>=1&&previous<=5?[1,2,3,4,5,2][scene]:previous;
 if(tune>=1){putInt(music.r,16,tune);putInt(music.r,20,1);putInt(music.r,24,tune<=5?2:-1);putInt(music.r,28,tune===1||tune>=3&&tune<=5?3:-1);putFloat(music.r,36,musicStarts[tune][0]);putFloat(music.r,40,musicStarts[tune][1]);putInt(music.r,64,musicOrders[tune]);putInt(music.r,68,tune===2?0x5c:musicOrders[tune]);music.r[72]=1}
 // A planted pot stops its decorative idle animation under the main plant.
 for(const pot of doc.pools[1].slots.filter(s=>(s.id>>>16)&&!s.r[333]&&int(s.r,48)===33)){
  if(doc.pools[1].slots.some(p=>(p.id>>>16)&&!p.r[333]&&int(p.r,52)===int(pot.r,52)&&int(p.r,36)===int(pot.r,36)&&![16,33].includes(int(p.r,48)))){
   const animation=doc.pools[9].slots.find(a=>a.id===uint(pot.r,160));if(animation)putFloat(animation.r,8,0);
  }
 }
 return encodeModel(core,doc);
}
