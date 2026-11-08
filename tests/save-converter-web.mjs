import fs from 'node:fs/promises';
import path from 'node:path';
import os from 'node:os';
import assert from 'node:assert/strict';
import {execFileSync} from 'node:child_process';
const {chromium}=await import(process.env.PVZ_PLAYWRIGHT_MODULE);
const game=path.resolve(process.argv[2]||'../PlantsVsZombiesModern');
const html=await fs.readFile('dist/web/save-converter.html','utf8');
const temporary=await fs.mkdtemp(path.join(os.tmpdir(),'pvz-web-converter-test-'));
const native=path.join(game,'tools/bin/windows-x64/pvz_dat64_converter.exe');
const source=path.join(game,'Tests/game3_13.dat'),profile=path.join(game,'out/x64-win32-vulkan/Release/data/user1.dat64');
const browser=await chromium.launch({headless:true,executablePath:'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'});
try{
 const page=await browser.newPage(),errors=[],requests=[];
 page.on('pageerror',error=>errors.push(String(error)));page.on('request',request=>requests.push(request.url()));
 await page.route('http://127.0.0.1:19999/',route=>route.fulfill({contentType:'text/html',body:html}));
 await page.goto('http://127.0.0.1:19999/');
 async function convert(input,target,expectedName){
  await page.locator('#file').setInputFiles(input);await page.locator('#target').selectOption(String(target));await page.locator('#round').check();
  await page.locator('#convert').click();
  await page.waitForFunction(()=>!document.getElementById('convert').disabled,null,{timeout:60000});
  assert.equal(await page.locator('#status').textContent(),'转换成功，原文件未修改。');
  const downloadPromise=page.waitForEvent('download');await page.locator('#download').click();const download=await downloadPromise;
  assert.equal(download.suggestedFilename(),expectedName);
  const destination=path.join(temporary,expectedName);await download.saveAs(destination);return destination;
 }
 const original=await fs.readFile(source);
 const current=await convert(source,4,'game3_58.dat64');
 const expected=path.join(temporary,'native-v4.dat64');execFileSync(native,['--target','dat64-v4',source,expected]);
 assert.deepEqual(await fs.readFile(current),await fs.readFile(expected));
 const back=await convert(current,0,'game3_13.dat');
 const expectedOriginal=path.join(temporary,'native-original.dat');execFileSync(native,['--target','legacy','--round-time','nearest',current,expectedOriginal]);
 assert.deepEqual(await fs.readFile(back),await fs.readFile(expectedOriginal));
 const userOriginal=await convert(profile,0,'user1.dat');
 const userBack=await convert(userOriginal,4,'user1.dat64');
 assert.deepEqual(await fs.readFile(userBack),await fs.readFile(profile));
 const rejected=Buffer.from(await fs.readFile(current));rejected.writeUInt32LE(2,8);
 await page.locator('#file').setInputFiles({name:'game3_58.dat64',mimeType:'application/octet-stream',buffer:rejected});
 await page.locator('#convert').click();await page.waitForFunction(()=>!document.getElementById('convert').disabled);
 assert.match(await page.locator('#status').textContent(),/转换失败/);assert.equal(await page.locator('#download').isVisible(),false);
 assert.deepEqual(await fs.readFile(source),original);assert.deepEqual(errors,[]);
 assert.deepEqual(requests,['http://127.0.0.1:19999/'],'Converter must not fetch Wasm or game assets');
 console.log('PASS isolated converter Wasm: original/v4 native byte parity, mode filename remapping, user roundtrip, malformed version rejection, no game runtime/assets, input preserved');
}finally{await browser.close()}
