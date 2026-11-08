import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const output=path.resolve(process.argv[2]||path.join(root,'dist/web/save-converter.html'));
const build=path.join(root,'out/web-converter');
await fs.mkdir(build,{recursive:true});
const emcc=process.env.EMXX||(process.platform==='win32'?'C:/UserProgram/DevTools/emsdk/upstream/emscripten/em++.exe':'em++');
const compiled=spawnSync(emcc,[path.join(root,'saveConverter/src/LevelSaveConverter.cpp'),path.join(root,'saveConverter/src/DataFixerUpper.cpp'),path.join(root,'saveConverter/web/WebEntry.cpp'),
 '-I'+path.join(root,'saveConverter/src'),'-I'+path.join(root,'saveConverter/contracts'),'-std=c++23','-sMEMORY64=0','-O3','-flto','-fexceptions',
 '-sDISABLE_EXCEPTION_CATCHING=0','-sMODULARIZE=1','-sEXPORT_NAME=createSaveConverter','-sENVIRONMENT=worker',
 '-sSINGLE_FILE=1','-sALLOW_MEMORY_GROWTH=1','-sINITIAL_MEMORY=16777216','-sMAXIMUM_MEMORY=536870912',
 '-sEXPORTED_RUNTIME_METHODS=["FS","ccall"]','-o',path.join(build,'converter-core.js')],{stdio:'inherit'});
if(compiled.status!==0)throw Error('存档转换器构建失败');
const core=await fs.readFile(path.join(build,'converter-core.js'),'utf8');
await fs.mkdir(path.dirname(output),{recursive:true});
await fs.writeFile(path.join(path.dirname(output),'save-tool-core.js'),core+'\nexport default createSaveConverter;\n');
const worker=await fs.readFile(path.join(root,'saveConverter/web/worker.js'),'utf8');
await fs.writeFile(path.join(path.dirname(output),'save-tool-worker.js'),"import createSaveConverter from './save-tool-core.js';\n"+worker);
await fs.copyFile(path.join(root,'saveConverter/web/client.mjs'),path.join(path.dirname(output),'save-tool-client.js'));
const html=(await fs.readFile(path.join(root,'saveConverter/web/index.html'),'utf8'))
 .replace('/* CONVERTER_CORE */',()=> (core+'\n'+worker).replaceAll('</script','<\\/script'));
await fs.mkdir(path.dirname(output),{recursive:true});await fs.writeFile(output,html);
const initialRecordBytes=await fs.readFile(path.join(root,'saveConverter/contracts/generation/initial-records-v4.json.gz'));
for(const [source,target] of [['model.mjs','save-model.js'],['prefab.mjs','save-prefab.js'],['generate.mjs','save-generate.js']]){
 let sourceText=await fs.readFile(path.join(root,'saveConverter/web',source),'utf8');sourceText=sourceText.replace('/* INITIAL_RECORD_DATA */ null',()=>JSON.stringify(initialRecordBytes.toString('base64'))).replaceAll('./model.mjs','./save-model.js').replaceAll('./prefab.mjs','./save-prefab.js');await fs.writeFile(path.join(path.dirname(output),target),sourceText);
}
await fs.rm(path.join(path.dirname(output),'save-initial-records-v4.json.gz'),{force:true});
console.log(output+' ('+Buffer.byteLength(html)+' bytes)');
