import fs from 'node:fs/promises';
import path from 'node:path';
import assert from 'node:assert/strict';
const {chromium}=await import(process.env.PVZ_PLAYWRIGHT_MODULE);
const game=path.resolve(process.argv[2]||'../PlantsVsZombiesModern');
const bundle=JSON.parse(await fs.readFile(path.join(game,'out/wasm-v3.3.8beta/toy/save-bundle.json'),'utf8'));
const profile=[...Buffer.from(bundle['user1.dat64'],'base64')],baseline=[...Buffer.from(bundle['game1_58.dat64'],'base64')];
const browser=await chromium.launch({headless:true,executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'});
try{
 const page=await browser.newPage(),requests=[],errors=[];
 page.on('request',request=>requests.push(request.url()));page.on('pageerror',error=>errors.push(String(error)));
 page.on('console',message=>{if(message.type()==='error'||message.type()==='warning')console.error(message.text().slice(0,3000))});
 await page.route('http://127.0.0.1:19997/**',async route=>{
  const name=new URL(route.request().url()).pathname.slice(1);
  if(!name)return route.fulfill({contentType:'text/html',body:'<!doctype html><title>Independent generator test</title>'});
  if(!['save-tool-core.js','save-tool-worker.js','save-tool-client.js','save-generate.js','save-model.js','save-prefab.js','save-initial-records-v4.json.gz'].includes(name))return route.abort();
  return route.fulfill({contentType:name.endsWith('.gz')?'application/gzip':'text/javascript',body:await fs.readFile('dist/web/'+name)});
 });
 await page.goto('http://127.0.0.1:19997/');
 const output=await page.evaluate(async({profile,baseline})=>{
  if(globalThis.Module)throw Error('Game module leaked into test');
  const {runIndependentSaveTool}=await import('./save-tool-client.js');
  const old=await runIndependentSaveTool({op:'extract',mode:58},new Uint8Array(baseline));
  const results=[];
  for(const scene of [2,0,1,3,4,5]){
   const cells=old.metadata.cells.map((cell,at)=>scene===2||scene===3||at<45?cell:0);
   const recipe={cells,flags:128,seed:114514,scene,rake:0,sun:4321};
   const start=performance.now();
   const result=await runIndependentSaveTool({op:'generate',mode:58,recipe},null,new Uint8Array(profile));
   const extracted=await runIndependentSaveTool({op:'extract',mode:58},result.bytes);
   if(JSON.stringify(result.metadata)!==JSON.stringify(extracted.metadata))throw Error('Generated metadata differs from re-extraction');
   results.push({scene,milliseconds:performance.now()-start,metadata:extracted.metadata,bytes:[...result.bytes]});
  }
  return results;
 },{profile,baseline});
 await fs.mkdir('out/generated-checkpoints',{recursive:true});
 for(const result of output){
  const metadata=result.metadata;
  assert.equal(metadata.flags,128);assert.equal(metadata.seed,114514);assert.equal(metadata.sun,4321);assert.equal(metadata.scene,result.scene);assert.equal(metadata.mowers,0);assert.equal(metadata.cards,0);assert.equal(metadata.repickPending,true);
  assert.equal(metadata.healthyPlants,metadata.plants);
  await fs.writeFile('out/generated-checkpoints/scene'+result.scene+'.dat64',new Uint8Array(result.bytes));
  console.log('PASS independent generator scene '+result.scene+': '+Math.round(result.milliseconds)+'ms, '+metadata.plants+' healthy plants, no mowers, 128 flags, seed 114514, sun 4321, chooser boundary');
 }
 assert.ok(!requests.some(url=>url.endsWith('.json.gz')),'Initial records must be embedded');assert.deepEqual(errors,[]);assert.ok(requests.every(url=>/^http:\/\/127\.0\.0\.1:19997\/(?:save-(?:(?:tool-(?:core|worker|client)|generate|model|prefab)\.js|initial-records-v4\.json\.gz))?$/.test(url)),'Unexpected game/assets request');
 console.log('PASS no game module, PAK, canvas, renderer, audio or decoder; all generated DAT64 documents passed complete independent validation');
}finally{await browser.close()}
