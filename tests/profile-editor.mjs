import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {decodeSummary,encodeSummary,newProfile,perfectProfile,decodeProfile,patchProfile} from '../saveEditor/profile-codec.mjs';
import {ProfileStore} from '../saveEditor/profile-store.mjs';
const empty=newProfile();assert.equal(empty.length,894);assert.equal(decodeProfile(empty).level,1);assert.deepEqual(patchProfile(empty,{}),empty);
const completed=new Uint8Array(await fs.readFile(path.join(process.env.PVZ_GAME_ROOT||fileURLToPath(new URL('../../PlantsVsZombiesModern/',import.meta.url)),'android/app/src/main/assets/bundled-saves/user1.dat64')));
assert.equal(decodeProfile(completed).coins,999990);assert.equal(decodeProfile(completed).purchases[29],1);assert.deepEqual(patchProfile(completed,{}),completed);
// Saving the displayed state must retain pending achievement notices and
// an exhausted-but-purchased consumable sentinel.
const pending=completed.slice(),pendingView=new DataView(pending.buffer);pendingView.setInt32(424+14*4,1000,true);pendingView.setUint8(829,0);
assert.deepEqual(patchProfile(pending,decodeProfile(pending)),pending);
const rows=[{id:1,name:'First',useSeq:1},{id:7,name:'Second',useSeq:2}];assert.deepEqual(decodeSummary(encodeSummary(rows)),rows);
assert.throws(()=>encodeSummary([{...rows[0]},{id:2,name:'first',useSeq:3}]),/重复/);assert.throws(()=>patchProfile(empty,{coins:15}),/10 的倍数/);assert.throws(()=>patchProfile(empty,{level:51}),/范围/);
// Two garden records and one Zombatar, with byte patterns in the opaque payloads.
const garden=new Uint8Array(894+176+72);garden.set(empty.subarray(0,828));const view=new DataView(garden.buffer);view.setInt32(824,2,true);garden.fill(73,828,1004);view.setInt32(1045,1,true);garden.fill(61,1049,1121);
const patched=patchProfile(garden,{coins:543210,level:35,finished:2});assert.equal(decodeProfile(patched).plants,2);assert.equal(decodeProfile(patched).zombatars,1);assert.deepEqual(patched.subarray(828),garden.subarray(828));
const files=new Map([['users.dat',encodeSummary(rows)],['user1.dat64',garden],['user7.dat64',completed],['game1_58.dat64',new Uint8Array([1,2,3])],['game1_0.dat64',new Uint8Array([4,5,6])],['game7_58.dat64',new Uint8Array([8,9])],['settings.json',new TextEncoder().encode('keep')]]);
const mem={readdir:()=>[...files.keys()],analyzePath:p=>({exists:files.has(p.split('/').at(-1))}),readFile:p=>files.get(p.split('/').at(-1))};
const store=new ProfileStore(mem,'/p'),apply=plan=>{for(const [name,bytes] of plan.writes)files.set(name,bytes);for(const name of plan.removes)if(!plan.writes.has(name))files.delete(name)};
assert.throws(()=>store.edit({oldId:1,id:7,name:'Collision',patch:{}}),/占用/);apply(store.edit({oldId:1,id:9,name:'Renamed',patch:{coins:100}}));
assert.equal(store.rows()[0].id,9);assert.equal(files.has('user1.dat64'),false);assert.deepEqual(files.get('game9_58.dat64'),new Uint8Array([1,2,3]));assert.deepEqual(files.get('game9_0.dat64'),new Uint8Array([4,5,6]));assert.deepEqual(files.get('user9.dat64').subarray(828),garden.subarray(828));assert.deepEqual(files.get('game7_58.dat64'),new Uint8Array([8,9]));
const added=store.add();apply(added);assert.equal(decodeProfile(files.get('user'+added.id+'.dat64')).finished,0);apply(store.remove(9));assert.equal(files.has('game9_58.dat64'),false);assert.equal(files.has('user7.dat64'),true);assert.equal(new TextDecoder().decode(files.get('settings.json')),'keep');
console.log('USER64 v13/profile summary: roundtrip, defaults, progress edits, opaque garden preservation, ID migration, collisions and deletion passed');

assert.deepEqual(perfectProfile(),completed,'perfect preset matches distributed profile byte for byte');
const beforePerfect=new Map([...files].map(([name,bytes])=>[name,bytes.slice()]));
const perfectPlan=store.add({perfect:true});assert.deepEqual(perfectPlan.removes,[]);apply(perfectPlan);
assert.deepEqual(files.get('user'+perfectPlan.id+'.dat64'),completed);
for(const [name,bytes] of beforePerfect)if(name!=='users.dat')assert.deepEqual(files.get(name),bytes,'preserved '+name);
const nextPerfect=store.add({perfect:true});assert.notEqual(nextPerfect.id,perfectPlan.id);apply(nextPerfect);
assert.equal(new Set(store.rows().map(p=>p.name.toLowerCase())).size,store.rows().length);
const altered=perfectProfile();altered[12]=50;assert.deepEqual(perfectProfile(),completed);
assert.equal(decodeProfile(newProfile()).finished,0);
console.log('perfect-user creation: distributed preset, independent bytes, unique IDs/names and existing saves preserved');

// HTML input bounds are advisory: validate the actual save operation too.
for(const count of [0,1,2,3,4]) {
 const purchases=decodeProfile(empty).purchases;purchases[21]=count;
 assert.equal(decodeProfile(patchProfile(empty,{purchases})).purchases[21],count);
}
for(const count of [-1,5,10,2147483647]) {
 const purchases=decodeProfile(empty).purchases;purchases[21]=count;
 const original=empty.slice();
 assert.equal(decodeProfile(patchProfile(empty,{purchases})).purchases[21],Math.max(0,Math.min(4,count)));
 assert.deepEqual(empty,original,'clamping must not mutate the original profile');
}
for(const count of [NaN,Infinity,2.5]) {
 const purchases=decodeProfile(empty).purchases;purchases[21]=count;
 assert.throws(()=>patchProfile(empty,{purchases}),/额外卡槽次数/);
}
console.log('seed-bank purchase bounds: valid counts preserved, out-of-range integers clamped, non-integers rejected');
