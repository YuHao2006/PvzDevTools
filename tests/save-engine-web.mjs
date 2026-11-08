import fs from 'node:fs/promises';
import path from 'node:path';
import assert from 'node:assert/strict';
const {chromium}=await import(process.env.PVZ_PLAYWRIGHT_MODULE);
const game=path.resolve(process.argv[2]||'../PlantsVsZombiesModern');
const bundle=JSON.parse(await fs.readFile(path.join(game,'out/wasm-v3.3.8beta/toy/save-bundle.json'),'utf8'));
const samples=[{mode:58,bytes:[...Buffer.from(bundle['game1_58.dat64'],'base64')]},
 {mode:0,bytes:[...await fs.readFile('C:/Users/Administrator/Downloads/game1_0.dat64')]}];
const browser=await chromium.launch({headless:true,executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'});
try{
 const page=await browser.newPage(),requests=[],errors=[];
 page.on('request',request=>requests.push(request.url()));page.on('pageerror',error=>errors.push(String(error)));
 await page.route('http://127.0.0.1:19998/**',async route=>{
  const name=new URL(route.request().url()).pathname.slice(1);
  if(!name)return route.fulfill({contentType:'text/html',body:'<!doctype html><title>Isolated save core test</title>'});
  if(!['save-tool-core.js','save-tool-worker.js','save-tool-client.js'].includes(name))return route.abort();
  return route.fulfill({contentType:'text/javascript',body:await fs.readFile('dist/web/'+name)});
 });
 await page.goto('http://127.0.0.1:19998/');
 const actual=await page.evaluate(async samples=>{
  if(globalThis.Module)throw Error('Game module leaked into isolated test');
  const {runIndependentSaveTool}=await import('./save-tool-client.js'),results=[];
   for(const sample of samples){
    const bytes=new Uint8Array(sample.bytes);
    const result=await runIndependentSaveTool({op:'extract',mode:sample.mode},bytes);
    if(bytes.length!==sample.bytes.length||!bytes.every((value,index)=>value===sample.bytes[index]))throw Error('Input was detached or modified');
    const validated=await runIndependentSaveTool({op:'validate',mode:sample.mode},bytes);
    if(validated.metadata.scene!==result.metadata.scene)throw Error('Validation metadata differs');
    results.push(result.metadata);
   }
   return results;
 },samples);
 const {decodeLineup,defaultLineups}=await import('../saveEditor/lineup-codec.mjs');
 assert.deepEqual(actual[0].cells,(await decodeLineup(defaultLineups[0].code)).cells);
 assert.equal(actual[0].sun,9990);assert.equal(actual[0].scene,2);
 assert.equal(actual[1].level,12);assert.equal(actual[1].scene,1);assert.equal(actual[1].music.tune,2);

 assert.deepEqual(errors,[]);
 assert.ok(requests.every(url=>/^http:\/\/127\.0\.0\.1:19998\/(?:save-tool-(?:core|worker|client)\.js)?$/.test(url)));
 console.log('PASS independent converter core: baseline lineup and reported night checkpoint metadata, validation parity, unchanged input; no game Module, Wasm, PAK, graphics or audio requested');
}finally{await browser.close()}
