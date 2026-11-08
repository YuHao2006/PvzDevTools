// Portable editor records, not native C++ object memory. All times stay BigInt.
export function readModel(bytes,schema){
 const view=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);let at=0;
 const u32=()=>{const n=view.getUint32(at,true);at+=4;return n};
 const block=()=>{const n=u32();if(n>bytes.length-at)throw Error('编辑模型被截断');const b=bytes.slice(at,at+n);at+=n;return b};
 if(new TextDecoder().decode(bytes.slice(0,8))!=='PVZMDL04')throw Error('编辑模型版本错误');at=8;
 const times=Array.from({length:u32()},()=>{const n=view.getBigUint64(at,true);at+=8;return n});let ti=0;
 const record=(fields,r=block())=>({r,t:fields.map(()=>times[ti++])});
 const doc={board:record(schema.fixed[0]),pools:[]};
 for(const poolSchema of schema.pools){const pool={free:u32(),used:u32(),size:u32(),slots:[]};if(pool.used>poolSchema.capacity)throw Error('编辑池超过容量');for(let i=0;i<pool.used;++i){const id=u32(),r=block(),v=block();pool.slots.push({...record(poolSchema.times,r),id,v})}doc.pools.push(pool)}
 const ids=()=>Array.from({length:u32()},u32);
 doc.systems=Array.from({length:u32()},()=>{const def=u32(),emitters=ids(),states=Array.from({length:u32()},()=>({def:u32(),image:u32(),particles:ids()}));return {def,emitters,states}});
 doc.animations=Array.from({length:u32()},()=>{const def=u32(),tracks=Array.from({length:u32()},()=>record(schema.fixed[1])),images=ids();return {def,tracks,images}});
 doc.fixed=schema.fixed.slice(2).map(fields=>record(fields));
 if(at!==bytes.length||ti!==times.length)throw Error('编辑模型长度不匹配');return doc;
}
export function writeModel(doc){
 const parts=[];const u32=n=>{const b=new Uint8Array(4);new DataView(b.buffer).setUint32(0,n,true);parts.push(b)};
 const block=b=>{u32(b.length);parts.push(b)};const records=[doc.board,...doc.pools.flatMap(p=>p.slots),...doc.animations.flatMap(a=>a.tracks),...doc.fixed];
 const times=records.flatMap(r=>r.t);parts.push(new TextEncoder().encode('PVZMDL04'));u32(times.length);
 for(const ns of times){const b=new Uint8Array(8);new DataView(b.buffer).setBigUint64(0,BigInt.asUintN(64,ns),true);parts.push(b)}block(doc.board.r);
 for(const pool of doc.pools){u32(pool.free);u32(pool.slots.length);u32(pool.slots.filter(s=>s.id>>>16).length);for(const slot of pool.slots){u32(slot.id);block(slot.r);block(slot.v)}}
 const ids=values=>{u32(values.length);for(const value of values)u32(value)};
 u32(doc.systems.length);for(const system of doc.systems){u32(system.def);ids(system.emitters);u32(system.states.length);for(const emitter of system.states){u32(emitter.def);u32(emitter.image);ids(emitter.particles)}}
 u32(doc.animations.length);for(const animation of doc.animations){u32(animation.def);u32(animation.tracks.length);for(const track of animation.tracks)block(track.r);ids(animation.images)}
 for(const record of doc.fixed)block(record.r);
 const out=new Uint8Array(parts.reduce((n,b)=>n+b.length,0));let at=0;for(const b of parts){out.set(b,at);at+=b.length}return out;
}
export const int=(b,at)=>new DataView(b.buffer,b.byteOffset,b.byteLength).getInt32(at,true);
export const uint=(b,at)=>new DataView(b.buffer,b.byteOffset,b.byteLength).getUint32(at,true);
export const float=(b,at)=>new DataView(b.buffer,b.byteOffset,b.byteLength).getFloat32(at,true);
export const putInt=(b,at,n)=>new DataView(b.buffer,b.byteOffset,b.byteLength).setInt32(at,n,true);
export const putFloat=(b,at,n)=>new DataView(b.buffer,b.byteOffset,b.byteLength).setFloat32(at,n,true);
export const putDouble=(b,at,n)=>new DataView(b.buffer,b.byteOffset,b.byteLength).setFloat64(at,n,true);
export function time(record,fields,name,value){const index=fields.findIndex(f=>f.name===name);if(index<0)throw Error('缺少时间字段 '+name);if(value!==undefined)record.t[index]=BigInt(value);return BigInt.asIntN(64,record.t[index])}
export function decodeModel(core,bytes){core.FS.writeFile('/input/document.dat64',bytes);const error=core.ccall('pvz_model_convert','string',['number'],[0]);if(error)throw Error(error);return readModel(core.FS.readFile('/output/document.model'),JSON.parse(core.ccall('pvz_model_schema','string',[],[])))}
export function encodeModel(core,doc){core.FS.writeFile('/input/document.model',writeModel(doc));const error=core.ccall('pvz_model_convert','string',['number'],[1]);if(error)throw Error(error);return core.FS.readFile('/output/generated.dat64')}
