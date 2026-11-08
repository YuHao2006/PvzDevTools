import {uint,int,putInt} from './model.mjs';
const live=pool=>pool.slots.filter(s=>s.id>>>16);
function supplements(doc){return {animations:new Map(live(doc.pools[9]).map((s,i)=>[s.id,doc.animations[i]])),systems:new Map(live(doc.pools[6]).map((s,i)=>[s.id,doc.systems[i]]))}}
function references(pool,slot,extra){
 const refs=[];const link=(p,at)=>{const id=uint(slot.r,at);if(id)refs.push({pool:p,id,bytes:slot.r,at})};
 if(pool===1){link(6,152);for(let at=160;at<=184;at+=4)link(9,at)}
 if(pool===5){link(9,60);link(6,64)}
 if(pool===7)link(7,120);
 if(pool===8)link(8,56);
 if(pool===10){const n=int(slot.r,768);if(n<0||n>16)throw Error('附件数量无效');for(let i=0;i<n;++i){const type=int(slot.r,i*48+4);if(type!==0&&type!==2)throw Error('不支持的附件种类');link(type===0?6:9,i*48)}}
 if(pool===9){if(!extra)throw Error('缺失动画补充数据');for(const track of extra.tracks){const id=uint(track.r,76);if(id)refs.push({pool:10,id,bytes:track.r,at:76})}}
 return refs;
}
export function selectPrefab(doc,pool,id){
 const extra=supplements(doc),lookup=doc.pools.map(p=>new Map(live(p).map(s=>[s.id,s]))),selected=doc.pools.map(()=>new Map());
 function visit(p,id){if(!id||selected[p].has(id))return;const slot=lookup[p].get(id);if(!slot)throw Error('模板含悬空引用 '+p+':'+id);selected[p].set(id,slot);const supplement=p===9?extra.animations.get(id):p===6?extra.systems.get(id):null;for(const ref of references(p,slot,supplement))visit(ref.pool,ref.id);
  if(p===6){for(const emitter of supplement.emitters)visit(7,emitter);for(const state of supplement.states)for(const particle of state.particles)visit(8,particle)}
 }
 visit(pool,id);
 return structuredClone({root:{pool,id},pools:selected.map(map=>[...map.values()]),animations:[...selected[9].keys()].map(id=>extra.animations.get(id)),systems:[...selected[6].keys()].map(id=>extra.systems.get(id))});
}
export function appendPrefab(doc,source,schema){
 const prefab=structuredClone(source),mapping=prefab.pools.map(()=>new Map());
 for(let p=0;p<prefab.pools.length;++p){const target=doc.pools[p];if(target.slots.length+prefab.pools[p].length>schema.pools[p].capacity)throw Error('生成存档超过 '+schema.pools[p].name+' 容量');for(const slot of prefab.pools[p]){const id=0x10000+target.slots.length;mapping[p].set(slot.id,id);target.slots.push(slot)}}
 const map=(p,id)=>{if(!id)return 0;const mapped=mapping[p].get(id);if(!mapped)throw Error('模板引用未重映射');return mapped};
 for(let p=0;p<prefab.pools.length;++p)for(const [i,slot]of prefab.pools[p].entries()){
  const extra=p===9?prefab.animations[i]:p===6?prefab.systems[i]:null;
  for(const ref of references(p,slot,extra))putInt(ref.bytes,ref.at,map(ref.pool,ref.id));
  slot.id=map(p,slot.id);
 }
 for(const system of prefab.systems){system.emitters=system.emitters.map(id=>map(7,id));for(const state of system.states)state.particles=state.particles.map(id=>map(8,id));doc.systems.push(system)}
 doc.animations.push(...prefab.animations);for(const pool of doc.pools){
  const holes=pool.slots.flatMap((slot,index)=>slot.id>>>16?[]:[index]);pool.free=holes[0]??pool.slots.length;
  for(let i=0;i<holes.length;++i)pool.slots[holes[i]].id=holes[i+1]??pool.slots.length;
  pool.used=pool.slots.length;pool.size=live(pool).length;
 }
 return {...prefab,root:{pool:source.root.pool,id:map(source.root.pool,source.root.id)}};
}
export function encodeCatalog(value){
 const buffers=[],indexes=new Map();const data=JSON.stringify(value,(_key,item)=>{
  if(typeof item==='bigint')return {$ns:item.toString()};
  if(item instanceof Uint8Array){const key=btoa(Array.from(item,n=>String.fromCharCode(n)).join(''));let index=indexes.get(key);if(index===undefined){index=buffers.length;buffers.push(key);indexes.set(key,index)}return {$bytes:index}}return item;
 });return JSON.stringify({buffers,data:JSON.parse(data)});
}
export function decodeCatalog(text){
 const envelope=JSON.parse(text),buffers=envelope.buffers.map(s=>Uint8Array.from(atob(s),c=>c.charCodeAt(0)));
 return JSON.parse(JSON.stringify(envelope.data),(_key,item)=>item?.$ns!==undefined?BigInt(item.$ns):item?.$bytes!==undefined?buffers[item.$bytes].slice():item);
}
