import fs from 'node:fs/promises';import path from 'node:path';
const files=['saveConverter/web/build.mjs','saveConverter/web/generate.mjs','saveConverter/web/model.mjs','saveConverter/web/prefab.mjs','saveConverter/web/worker.js','saveEditor/standalone.mjs'];
for(const name of files){const text=await fs.readFile(name,'utf8');if(/(?:PlantsVsZombiesModern|compile_commands|\.cpp\.o|LawnApp\.h|Lawn\/Board\.h|window\.Module|PVZSaveToolJob|save-generator-core)/.test(text))throw Error('Game dependency in standalone tool: '+name)}
const build=await fs.readFile(files[0],'utf8');if(!build.includes('-sMEMORY64=0'))throw Error('Tool memory model is not explicitly wasm32');
const dist=await fs.readdir('dist/web');if(dist.includes('save-generator-core.js'))throw Error('Retired game-linked prototype was packaged');
const text=await fs.readFile('dist/web/save-tool-core.js','utf8');for(const symbol of ['RunWebSaveTool','LawnApp','SDL_CreateWindow','glDrawElements','wgpuDeviceCreate'])if(text.includes(symbol))throw Error('Game/renderer symbol leaked: '+symbol);
console.log('PASS standalone dependency boundary and explicit wasm32 build; no game objects, PAK, renderer, audio or retired prototype');
