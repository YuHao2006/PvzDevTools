// The editor's worker owns its own converter instance and memory. It never
// reads a game Module, IDBFS, graphics adapter, audio decoder or package.
let worker, pending=false;
export async function runIndependentSaveTool(request,input,profile){
 if(pending)throw Error('离线存档工具正在处理上一项任务');
 if(!['generate','extract','validate','repair-plant-attacks','dfu'].includes(request?.op))throw Error('独立存档工具尚未实现此操作：'+request?.op);
 const mode=request.mode??58;
 if(!Number.isInteger(mode)||mode<0||mode>72)throw Error('关卡模式无效');
 if(request.op!=='generate'&&!(input instanceof Uint8Array))throw Error('缺少 DAT64 存档数据');
 if(request.op==='generate'&&!(profile instanceof Uint8Array))throw Error('缺少用户资料');
 pending=true;
 try{
  worker??=new Worker(new URL('./save-tool-worker.js',import.meta.url),{type:'module'});
  const bytes=input?.slice(),details=profile?.slice(); // Never detach caller-owned data.
  return await new Promise((resolve,reject)=>{
   worker.onmessage=({data})=>data.error?reject(Error(data.error)):resolve(data);
   worker.onerror=event=>{worker.terminate();worker=null;reject(Error(event.message||'独立存档工具运行失败'))};
   worker.postMessage({name:'game1_'+mode+'.dat64',bytes,profile:details,request:{...request,mode}},[bytes,details].filter(Boolean).map(value=>value.buffer));
  });
 }finally{pending=false}
}

// Startup DFU must not retain a second wasm heap throughout the game session.
export function releaseIndependentSaveTool(){
 if(pending)return false;
 worker?.terminate();worker=undefined;return true;
}
