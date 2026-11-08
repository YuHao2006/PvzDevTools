// Independent browser file store. No game Module, MEMFS, PAK or game Wasm.
import {ProfileStore,modes} from './save-codec.js';
import {LineupLibrary} from './lineup-library.js';
import {defaultLineups,decodeLineup,encodeLineup,makeRecipe} from './save-lineups.js';
import {runIndependentSaveTool} from './save-tool-client.js';
export async function createStandalone(){
 const db=await new Promise((resolve,reject)=>{const open=indexedDB.open('pvz_devtools_save_editor_v4',1);open.onupgradeneeded=()=>open.result.createObjectStore('files');open.onsuccess=()=>resolve(open.result);open.onerror=()=>reject(open.error)});
 const saved=await new Promise((resolve,reject)=>{const tx=db.transaction('files','readonly'),request=tx.objectStore('files').get('snapshot');request.onsuccess=()=>resolve(request.result||[]);request.onerror=()=>reject(request.error)});
 const files=new Map(saved.map(([name,bytes])=>[name,new Uint8Array(bytes)])),root='/data';
 const validPath=path=>{if(!path.startsWith(root+'/')||path.slice(root.length+1).includes('/'))throw Error('存档路径无效');return path.slice(root.length+1)};
 const fs={readdir:path=>{if(path!==root)throw Error('存档目录无效');return ['.','..',...files.keys()]},analyzePath:path=>({exists:files.has(validPath(path))}),readFile:path=>{const value=files.get(validPath(path));if(!value)throw Error('存档文件不存在');return value.slice()},writeFile:(path,bytes)=>files.set(validPath(path),bytes.slice()),unlink:path=>files.delete(validPath(path))};
 const store=new ProfileStore(fs,root),library=new LineupLibrary(localStorage,defaultLineups,{key:'pvz_devtools_lineup_library_v1',customKey:'pvz_devtools_lineups'});
 const persist=()=>new Promise((resolve,reject)=>{const tx=db.transaction('files','readwrite');tx.objectStore('files').put([...files].map(([name,bytes])=>[name,bytes]),'snapshot');tx.oncomplete=resolve;tx.onerror=()=>reject(tx.error);tx.onabort=()=>reject(tx.error||Error('本地保存被中断'))});
 const names=()=>[...files.keys()].filter(name=>/^game[1-9]\d*_\d+\.dat64$/.test(name)).sort(),listed=()=>({presets:library.snapshot().lineups,custom:[]});
 const slot=name=>{const match=/^game([1-9]\d*)_(\d+)\.dat64$/.exec(name);if(!match||!modes[Number(match[2])]?.checkpoint)throw Error('关卡文件名无效，见“存档文件名对照”');const id=Number(match[1]);if(!store.rows().some(row=>row.id===id))throw Error('请先新增目标用户');return {id,mode:Number(match[2])}};
 const read=name=>{slot(name);const bytes=files.get(name);if(!bytes)throw Error('没有选中的关卡存档');return bytes.slice()};
 async function commit(plan){const before=new Map(files);try{for(const [name,bytes]of plan.writes)files.set(name,bytes.slice());for(const name of plan.removes)if(!plan.writes.has(name))files.delete(name);await persist()}catch(error){files.clear();for(const [name,bytes]of before)files.set(name,bytes);throw error}return {id:plan.id,profiles:store.list(),saves:names()}}
 if(!files.has('users.dat'))await commit(store.add());
 return {
  async initial(){const entries=listed(),first=entries.presets[0],lineup=first?await decodeLineup(first.code):{scene:2,rake:0,cells:Array(54).fill(0)};return {...entries,lineup,code:first?.code||await encodeLineup(lineup),profiles:store.list(),saves:names()}},
  async request(op,payload={}){
   if(op==='close')return {};
   if(op==='add-user')return commit(store.add({perfect:payload.perfect===true}));
   if(op==='edit-user')return commit(store.edit(payload));
   if(op==='delete-user')return commit(store.remove(payload.id));
   if(op==='export-users')return {files:[...files].filter(([name])=>/^(?:users\.dat|user[1-9]\d*\.dat64)$/.test(name)).map(([name,bytes])=>[name,bytes.slice()])};
   if(op==='delete-lineup'){library.remove(payload.code);return listed()}
   if(op==='save-lineup'){const decoded=await decodeLineup(payload.code);library.add({name:String(payload.name||'我的阵型').trim().slice(0,60),code:decoded.code});return listed()}
   if(op==='replace-lineups'){for(const entry of payload.lineups||[])await decodeLineup(entry.code);library.replace(payload);return listed()}
   const target=slot(payload.name);
   if(op==='export')return {bytes:read(payload.name)};
   if(op==='extract'||op==='repair-plant-attacks')return runIndependentSaveTool({op,mode:target.mode},read(payload.name));
   if(op==='import'){if(!(payload.bytes instanceof Uint8Array)||payload.bytes.length>32*1024*1024)throw Error('DAT64 文件无效');await runIndependentSaveTool({op:'validate',mode:target.mode},payload.bytes);return commit({writes:new Map([[payload.name,payload.bytes]]),removes:[]})}
   if(op==='generate'){
    const r=payload.recipe,recipe=makeRecipe(r,r.flags,r.seed,r.scene,r.sun),result=await runIndependentSaveTool({op:'generate',mode:target.mode,recipe},null,store.details(target.id));
    return commit({writes:new Map([[payload.name,result.bytes]]),removes:[]});
   }
   throw Error('不支持的工具操作');
  }
 };
}
