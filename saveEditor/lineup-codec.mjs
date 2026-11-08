export const defaultLineups=Object.freeze([
 Object.freeze({name:'97无炮',code:'LI5HzH3sQVwhTSBNuEyUFDT5tA7ETJcC0uEUXFBaf5WEmpVyND+UDkTQ3ox0EGQhXXQY0lZUnQpLX1Y='}),
 Object.freeze({name:'14格',code:'LI43NBRTVj9Rg/bebPggzF2Qf01MVoKWEZQ2VT9XtD6FITVWVIO+WA1X'}),
 Object.freeze({name:'24炮',code:'LI43bJyUlNTYBS00RdPXWnxsNHHS3FlXRbJUVHQGQ8pW'}),
 Object.freeze({name:'[终极战术]五角星无炮流',code:'LI43jH/kRlzjZrhRF3RUpjJeOFHFZEXTHtMe0tlawVVE7GI0efJc8npSWMdVLhV261Y='})
]);
// Toolkit 1.22 format: packed 16-bit cells, zlib stream, trailing scene/rake,
// XOR 0x54, then Base64. Format reference: lmintlcx/pvztoolkit/src/lineup.cpp.
export async function decodeLineup(value){
 const code=value.trim().replace(/\s+/g,'').replace(/-/g,'+').replace(/_/g,'/');
 if(!/^[A-Za-z0-9+/]{16,164}={0,2}$/.test(code)||code.length%4||code.length>164)throw Error('阵型码格式错误');
 const bytes=Uint8Array.from(atob(code),c=>c.charCodeAt(0)^0x54),meta=bytes.at(-1),scene=meta&15,rake=meta>>>4;
 const rows=scene===2||scene===3?6:5;
 if(scene>5||rake>rows||((scene===2||scene===3)&&(rake===3||rake===4)))throw Error('阵型码的场景／钉耙位置无效');
 const reader=new Blob([bytes.slice(0,-1)]).stream().pipeThrough(new DecompressionStream('deflate')).getReader();
 const chunks=[];let length=0;
 try{for(;;){const {value,done}=await reader.read();if(done)break;length+=value.length;if(length>108)throw Error('阵型码数据过长');chunks.push(value)}}catch(error){throw Error('阵型码解压失败：'+(error.message||error))}finally{try{await reader.cancel()}catch{}}
 if(length!==rows*18)throw Error('阵型码格子数量与场景不符');
 const raw=new Uint8Array(length);let offset=0;for(const chunk of chunks){raw.set(chunk,offset);offset+=chunk.length}
 const view=new DataView(raw.buffer),cells=Array.from({length:54},(_,i)=>i<rows*9?view.getUint16(i*2,true):0);
 if(cells.some(c=>(c>>>10)>48))throw Error('阵型码包含不支持的植物类型');
 return {code,scene,rake,cells};
}
export function makeRecipe(lineup,flags,seed,scene,sun=9990){
 if(!Number.isInteger(flags)||flags<0||flags>10000000||flags%2)throw Error('已完成旗数须为0～10000000之间的偶数');
 if(!Number.isInteger(seed)||seed<-2147483648||seed>2147483647)throw Error('随机数种子须为32位整数');
 if(!Number.isInteger(scene)||scene<0||scene>5)throw Error('无效场景');
 if(!Number.isInteger(sun)||sun<0||sun>9990)throw Error('阳光须为0～9990之间的整数');
 const rows=scene===2||scene===3?6:5;
 if(lineup.cells.slice(rows*9).some(Boolean))throw Error('该阵型的第六排有物体，不能用于五排场景');
 if(lineup.rake>rows||((scene===2||scene===3)&&(lineup.rake===3||lineup.rake===4)))throw Error('所选场景不支持阵型中的钉耙位置');
 return {flags,seed,scene,sun,rake:lineup.rake,cells:lineup.cells};
}

export async function encodeLineup({scene,rake,cells}){
 const rows=scene===2||scene===3?6:5;
 const raw=new Uint8Array(rows*18),view=new DataView(raw.buffer);
 for(let i=0;i<rows*9;i++)view.setUint16(i*2,cells[i],true);
 const packed=new Uint8Array(await new Response(new Blob([raw]).stream().pipeThrough(new CompressionStream('deflate'))).arrayBuffer());
 const bytes=new Uint8Array(packed.length+1);bytes.set(packed);bytes[packed.length]=(rake<<4)|scene;
 return btoa(String.fromCharCode(...bytes.map(x=>x^0x54)));
}
