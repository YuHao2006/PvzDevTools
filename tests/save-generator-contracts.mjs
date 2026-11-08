import fs from 'node:fs/promises';import assert from 'node:assert/strict';import path from 'node:path';
const {chromium}=await import(process.env.PVZ_PLAYWRIGHT_MODULE),root=path.resolve('dist/web');const browser=await chromium.launch({headless:true,executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'});
try{const page=await browser.newPage(),requests=[],errors=[];page.on('pageerror',e=>errors.push(e.message));page.on('request',r=>requests.push(r.url()));await page.route('http://localhost:19994/**',async route=>{const name=new URL(route.request().url()).pathname.slice(1);if(!name)return route.fulfill({contentType:'text/html',body:'Standalone contract test'});try{return route.fulfill({contentType:name.endsWith('.gz')?'application/gzip':'text/javascript',body:await fs.readFile(path.join(root,name))})}catch{return route.abort()}});await page.goto('http://localhost:19994/');
 const results=await page.evaluate(async()=>{
  const {runIndependentSaveTool}=await import('./save-tool-client.js'),{perfectProfile}=await import('./save-codec.js');
  HTMLCanvasElement.prototype.getContext=()=>{throw Error('Tool requested a renderer')};window.AudioContext=class{constructor(){throw Error('Tool requested audio')}};Object.defineProperty(window,'Module',{get(){throw Error('Tool requested game Module')}});
  const profile=perfectProfile(),out=[];
  for(const scene of [0,1,2,3,4,5]){
   for(const asleep of [false,true]){
    const rows=scene===2||scene===3?6:5,cells=Array(54).fill(0);for(let at=0;at<rows*9;++at){const seeds=Array.from({length:48},(_,i)=>i).filter(i=>![16,30,33,35].includes(i)),seed=seeds[at%seeds.length];cells[at]=((seed+1)<<10)|(asleep?0:256)|512|(at%2?128:64)|(at%3===0?16:0)|(at%7===0?4:0);}
    const result=await runIndependentSaveTool({op:'generate',mode:58,recipe:{scene,cells,flags:128,seed:114514,rake:1,sun:3210}},null,profile);
    const extracted=await runIndependentSaveTool({op:'extract',mode:58},result.bytes);if(JSON.stringify(extracted.metadata.cells)!==JSON.stringify(cells))throw Error('Cell mismatch scene '+scene);out.push({scene,asleep,plants:result.metadata.plants});
   }
  }
  for(let mode=0;mode<=72;++mode){if([34,35,71].includes(mode))continue;const cells=Array(54).fill(0);cells[0]=(48<<10)|256;cells[9]=(19<<10)|256;const result=await runIndependentSaveTool({op:'generate',mode,level:12,recipe:{scene:2,cells,flags:128,seed:-2147483648,rake:0,sun:9990}},null,profile);if(!result.bytes.length)throw Error('Missing mode '+mode);out.push({mode,size:result.bytes.length})}
  return out;
 });assert.deepEqual(errors,[]);console.log('PASS '+results.length+' independent generation cases: six scenes × awake/asleep, multi-head and attached effects, all battle modes, canonical model validation; game/graphics/audio disabled');assert.ok(requests.every(u=>/^http:\/\/localhost:19994\/(?:save-[^/]+|lineup-library\.js)?$/.test(u)));await fs.writeFile('out/web-generator-contracts.json',JSON.stringify(results,null,2));
}finally{await browser.close()}
