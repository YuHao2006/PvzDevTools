// Frozen USER64 v13 / users.dat v14 contract. Existing garden/Zombatar bytes
// are retained verbatim; editing these fields never constructs a game object.
const magic='PVZUSR64',encoder=new TextEncoder(),decoder=new TextDecoder();
function require(value,message){if(!value)throw Error(message)}
function integer(value,min,max,label){require(Number.isInteger(value)&&value>=min&&value<=max,label+'超出允许范围');return value}
export function validName(value){require(typeof value==='string'&&/^[\x20-\x7e]{1,20}$/.test(value)&&value.trim()===value,'用户名需为 1～20 个 ASCII 字符，首尾不能有空格');return value}
export function validId(value){return integer(value,1,2147483646,'用户 ID')}
export function decodeSummary(bytes){
 const view=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);let at=0;
 const read=(size,method)=>{require(at+size<=bytes.length,'用户列表不完整');const value=view[method](at,true);at+=size;return value};
 require(read(4,'getUint32')===14,'仅支持 users.dat v14');const count=read(2,'getUint16'),profiles=[];require(count<=1000,'用户数量异常');
 for(let i=0;i<count;i++){const size=read(2,'getUint16');require(at+size<=bytes.length,'用户名不完整');const name=decoder.decode(bytes.subarray(at,at+size));at+=size;const useSeq=read(4,'getUint32'),id=validId(read(4,'getUint32'));profiles.push({name,useSeq,id})}
 require(at===bytes.length,'用户列表有多余数据');validateProfiles(profiles);return profiles;
}
export function validateProfiles(profiles){require(Array.isArray(profiles)&&profiles.length<=1000,'用户列表无效');const ids=new Set(),names=new Set();for(const p of profiles){validId(p.id);validName(p.name);integer(p.useSeq,0,4294967294,'使用序号');require(!ids.has(p.id)&&!names.has(p.name.toLowerCase()),'用户 ID 或名称重复');ids.add(p.id);names.add(p.name.toLowerCase())}}
export function encodeSummary(profiles){validateProfiles(profiles);const bytes=new Uint8Array(6+profiles.reduce((n,p)=>n+10+encoder.encode(p.name).length,0)),v=new DataView(bytes.buffer);v.setUint32(0,14,true);v.setUint16(4,profiles.length,true);let at=6;for(const p of profiles){const name=encoder.encode(p.name);v.setUint16(at,name.length,true);bytes.set(name,at+2);at+=2+name.length;v.setUint32(at,p.useSeq,true);v.setUint32(at+4,p.id,true);at+=8}return bytes}
function layout(bytes){require(bytes instanceof Uint8Array&&bytes.length>=894&&decoder.decode(bytes.subarray(0,8))===magic,'用户明细不是有效的 USER64');const v=new DataView(bytes.buffer,bytes.byteOffset,bytes.byteLength);require(v.getInt32(8,true)===13,'仅支持 USER64 v13');const plants=v.getInt32(824,true);integer(plants,0,200,'花园植物数');const achievements=828+plants*88;require(achievements+45<=bytes.length,'花园数据不完整');const zombatars=v.getInt32(achievements+41,true);integer(zombatars,0,100,'大头贴数');require(bytes.length===894+plants*88+zombatars*72,'用户明细长度不正确');return {v,achievements,mini:achievements+45+zombatars*72,plants,zombatars}}
export function decodeProfile(bytes){const {v,achievements,plants,zombatars}=layout(bytes);return {level:v.getInt32(12,true),coins:v.getInt32(16,true)*10,finished:v.getInt32(20,true),records:Array.from({length:100},(_,i)=>v.getInt32(24+i*4,true)),purchases:Array.from({length:80},(_,i)=>v.getInt32(424+i*4,true)),unlocks:[8,9,14].map(i=>v.getInt32(744+i*4,true)!==0),achievements:Array.from({length:20},(_,i)=>v.getUint8(achievements+i*2)!==0),plants,zombatars}}
export function newProfile(){const bytes=new Uint8Array(894);bytes.set(encoder.encode(magic));const v=new DataView(bytes.buffer);v.setInt32(8,13,true);v.setInt32(12,1,true);return bytes}
// Same completed USER64 preset as pvz_perf's distributed perfect profile.
// Always create fresh bytes: adding a user must never alter another user's data.
export function perfectProfile(){
 const records=Array(100).fill(0),purchases=Array(80).fill(0);
 for(let mode=1;mode<=70;mode++)if(mode!==35)records[mode]=1;
 for(let mode=61;mode<=65;mode++)records[mode]=10;
 for(let mode=66;mode<=70;mode++)records[mode]=5;
 records[71]=1000;
 for(const item of [0,1,2,3,4,5,6,7,8,13,16,17,18,19,20,22,23,25,27,29])purchases[item]=1;
 purchases[9]=2;purchases[21]=4;purchases[24]=3;
 for(const item of [14,15])purchases[item]=1020;
 for(const item of [26,28])purchases[item]=1010;
 const bytes=patchProfile(newProfile(),{level:1,coins:999990,finished:2,records,purchases,unlocks:[true,true,true],achievements:Array(20).fill(true)}),{v,mini}=layout(bytes);
 // Already seen Stinky and upsell; suppress the first-use Zombatar notice.
 v.setInt32(812,1,true);v.setInt32(816,1,true);v.setUint8(mini+20,1);
 return bytes;
}
export function patchProfile(original,patch){
 const bytes=original.slice(),{v,achievements,mini}=layout(bytes),previous=new DataView(original.buffer,original.byteOffset,original.byteLength);
 if('level'in patch)v.setInt32(12,integer(patch.level,1,50,'冒险关卡'),true);
 if('coins'in patch){integer(patch.coins,0,999990,'金币');require(patch.coins%10===0,'金币需为 10 的倍数');v.setInt32(16,patch.coins/10,true)}
 if('finished'in patch)v.setInt32(20,integer(patch.finished,0,2147483647,'冒险通关次数'),true);
 if(patch.records){require(Array.isArray(patch.records)&&patch.records.length===100,'挑战记录数组无效');patch.records.forEach((value,i)=>v.setInt32(24+i*4,integer(value,0,2147483647,'挑战记录'),true));for(let i=0;i<20;i++)if(patch.records[i+1]!==previous.getInt32(24+(i+1)*4,true))v.setUint8(mini+i,patch.records[i+1]>0?1:0)}
 if(patch.purchases){require(Array.isArray(patch.purchases)&&patch.purchases.length===80,'商店记录数组无效');patch.purchases.forEach((value,i)=>v.setInt32(424+i*4,integer(value,0,2147483647,'购买记录'),true))}
 if(patch.unlocks){require(Array.isArray(patch.unlocks)&&patch.unlocks.length===3,'解锁状态无效');[8,9,14].forEach((field,i)=>v.setInt32(744+field*4,patch.unlocks[i]?1:0,true))}
 if(patch.achievements){require(Array.isArray(patch.achievements)&&patch.achievements.length===20,'成就状态无效');patch.achievements.forEach((earned,i)=>{if(!!earned===(v.getUint8(achievements+i*2)!==0))return;v.setUint8(achievements+i*2,earned?1:0);v.setUint8(achievements+i*2+1,earned?1:0)})}
 return bytes;
}
