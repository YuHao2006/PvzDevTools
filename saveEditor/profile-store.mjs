import {decodeSummary,encodeSummary,decodeProfile,newProfile,patchProfile,validName,validId,validateProfiles} from './profile-codec.mjs';
// Build a complete mutation plan first. The host commits it only while stopped.
export class ProfileStore {
 constructor(fs,root){this.fs=fs;this.root=root}
 names(){return this.fs.readdir(this.root).filter(n=>n!=='.'&&n!=='..')}
 rows(){return this.fs.analyzePath(this.root+'/users.dat').exists?decodeSummary(this.fs.readFile(this.root+'/users.dat')):[]}
 details(id){const path=this.root+'/user'+id+'.dat64';return this.fs.analyzePath(path).exists?this.fs.readFile(path):newProfile()}
 list(){return this.rows().map(row=>({...row,progress:decodeProfile(this.details(row.id))}))}
 owned(id){return this.names().filter(name=>name==='user'+id+'.dat64'||new RegExp('^game'+id+'_\\d+\\.dat64$').test(name))}
 add(){const rows=this.rows();let id=1;while(rows.some(p=>p.id===id)||this.owned(id).length)++id;validId(id);let name='Player'+id;while(rows.some(p=>p.name.toLowerCase()===name.toLowerCase()))name+='x';validName(name);rows.push({id,name,useSeq:Math.max(0,...rows.map(p=>p.useSeq))+1});validateProfiles(rows);return {id,writes:new Map([['user'+id+'.dat64',newProfile()],['users.dat',encodeSummary(rows)]]),removes:[]}}
 edit({oldId,id,name,patch}){validId(oldId);validId(id);validName(name);const rows=this.rows(),user=rows.find(p=>p.id===oldId);if(!user)throw Error('目标用户不存在');if(id!==oldId&&(rows.some(p=>p.id===id)||this.owned(id).length))throw Error('目标 ID 已被用户或关卡存档占用');const details=patchProfile(this.details(oldId),patch);user.id=id;user.name=name;validateProfiles(rows);const writes=new Map(),removes=[];if(id!==oldId)for(const file of this.owned(oldId)){removes.push(file);if(file.startsWith('game'))writes.set(file.replace('game'+oldId+'_','game'+id+'_'),this.fs.readFile(this.root+'/'+file))}writes.set('user'+id+'.dat64',details);writes.set('users.dat',encodeSummary(rows));return {id,writes,removes}}
 remove(id){validId(id);const rows=this.rows();if(!rows.some(p=>p.id===id))throw Error('目标用户不存在');return {writes:new Map([['users.dat',encodeSummary(rows.filter(p=>p.id!==id))]]),removes:this.owned(id)}}
}
