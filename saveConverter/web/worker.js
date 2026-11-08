let engine,generator;
onmessage=async({data})=>{
 try{
  engine??=Promise.resolve().then(()=>createSaveConverter()).catch(error=>{engine=null;throw Error('存档格式核心初始化失败：'+(error.message||error))});
  const module=await engine;
  for(const dir of ['/input','/output']){
   try{module.FS.mkdir(dir)}catch(error){if(!module.FS.analyzePath(dir).exists)throw error}
   for(const name of module.FS.readdir(dir))if(name!=='.'&&name!=='..')module.FS.unlink(dir+'/'+name);
  }
  if(data.request?.op==='generate'){
   generator??=import('./save-generate.js').catch(error=>{generator=null;throw Error('存档生成器模块加载失败（save-generate.js）：'+(error.message||error))});
   const {generateSave}=await generator;
   let bytes=await generateSave(module,data.request,new Uint8Array(data.profile));
   if(data.request.format!==4){
    module.FS.writeFile("/input/generated-v4.dat64",bytes);
    const converted=module.ccall("pvz_convert_save","string",["string","number","number","number"],["generated-v4.dat64",5,0,0]);
    if(converted)throw Error(converted);
    bytes=module.FS.readFile("/output/generated-v4.dat64");
   }
   module.FS.writeFile('/input/generated.dat64',bytes);
   const invalid=module.ccall('pvz_inspect_save','string',['string','number','number'],['generated.dat64',data.request.mode,0]);
   if(invalid)throw Error('生成存档未通过独立校验：'+invalid);
   const metadata=JSON.parse(module.ccall('pvz_save_metadata','string',[],[]));
   postMessage({metadata,bytes},[bytes.buffer]);return;
  }
  module.FS.writeFile('/input/'+data.name,new Uint8Array(data.bytes));
  if(data.request){
   const {op,mode}=data.request;
   if(op==='dfu'){
    const error=module.ccall('pvz_dfu_save','string',['string'],[data.name]);
    if(error)throw Error(error);
    const name=module.ccall('pvz_conversion_output_name','string',[],[]);
    if(name){const bytes=module.FS.readFile('/output/'+name);postMessage({bytes},[bytes.buffer])}
    else postMessage({unchanged:true});
    return;
   }
   if(!['extract','validate','repair-plant-attacks'].includes(op))throw Error('独立存档工具暂不支持此操作：'+op);
   const error=module.ccall('pvz_inspect_save','string',['string','number','number'],[data.name,mode,+(op==='repair-plant-attacks')]);
   if(error)throw Error(error);
   const metadata=JSON.parse(module.ccall('pvz_save_metadata','string',[],[]));
   const name=module.ccall('pvz_conversion_output_name','string',[],[]);
   if(name){const bytes=module.FS.readFile('/output/'+name);postMessage({metadata,bytes},[bytes.buffer])}
   else postMessage({metadata});
   return;
  }
  const error=module.ccall('pvz_convert_save','string',['string','number','number','number'],[data.name,data.target,+data.round,+data.user]);
  if(error)throw Error(error);
  const name=module.ccall('pvz_conversion_output_name','string',[],[]);
  const bytes=module.FS.readFile('/output/'+name);
  postMessage({name,bytes},[bytes.buffer]);
 }catch(error){postMessage({error:error.message||String(error)})}
 finally{
  if(engine)try{
   const module=await engine;
   for(const dir of ['/input','/output'])for(const name of module.FS.readdir(dir))if(name!=='.'&&name!=='..')module.FS.unlink(dir+'/'+name);
  }catch{}
 }
};
