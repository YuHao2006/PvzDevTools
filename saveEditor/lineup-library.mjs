// Portable JSON library. Codes are identities; list positions are never IDs.
export function normalizeLineupLibrary(value){
 if(value?.format!==1||!Array.isArray(value.lineups)||value.lineups.length>2000)throw Error('阵型库 JSON 格式无效');
 const seen=new Set(),lineups=[];
 for(const entry of value.lineups){
  if(!entry||typeof entry.name!=='string'||typeof entry.code!=='string')throw Error('阵型库条目无效');
  const name=entry.name.trim(),code=entry.code.trim().replace(/\s+/g,'').replace(/-/g,'+').replace(/_/g,'/');
  if(!name||name.length>60||! /^[A-Za-z0-9+/]{16,164}={0,2}$/.test(code)||code.length%4)throw Error('阵型名称或阵型码无效');
  if(!seen.has(code)){seen.add(code);lineups.push({name,code})}
 }
 return {format:1,lineups};
}
export class LineupLibrary {
 constructor(storage,defaults=[],{key='pvz_lineup_library_v1',customKey='pvz_custom_lineups',deletedKey='pvz_deleted_lineups'}={}){
  Object.assign(this,{storage,key});
  const current=storage.getItem(key),custom=storage.getItem(customKey),deleted=storage.getItem(deletedKey);
  this.existed=current!==null||custom!==null||deleted!==null;
  if(current!==null)this.value=normalizeLineupLibrary(JSON.parse(current));
  else{
   const added=custom===null?[]:JSON.parse(custom),removed=deleted===null?[]:JSON.parse(deleted);
   if(!Array.isArray(added)||!Array.isArray(removed))throw Error('旧阵型列表格式无效');
   this.value=normalizeLineupLibrary({format:1,lineups:[...defaults.filter(p=>!removed.includes(p.code)),...added]});
   if(this.existed)this.replace(this.value);
  }
 }
 snapshot(){return normalizeLineupLibrary(this.value)}
 replace(value){const next=normalizeLineupLibrary(value);this.storage.setItem(this.key,JSON.stringify(next));this.value=next;this.existed=true}
 add(entry){const next=this.snapshot();if(!next.lineups.some(p=>p.code===entry.code))next.lineups.push(entry);this.replace(next)}
 remove(code){this.replace({format:1,lineups:this.value.lineups.filter(p=>p.code!==code)})}
}
