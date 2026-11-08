import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const source=path.join(root,'saveEditor');
const codec=await fs.readFile(path.join(source,'profile-codec.mjs'),'utf8');
const store=await fs.readFile(path.join(source,'profile-store.mjs'),'utf8');
const modes=await fs.readFile(path.join(source,'modes.json'),'utf8');
const profileUI=await fs.readFile(path.join(source,'profile-ui.js'),'utf8');
const library=await fs.readFile(path.join(source,'lineup-library.mjs'),'utf8');
const exports=['validName','validId','decodeSummary','encodeSummary','validateProfiles','decodeProfile','newProfile','perfectProfile','patchProfile'];
const script="const Profiles=(()=>{\n"+codec.replaceAll('export ','')+'\nreturn {'+exports.join(',')+'};})();\n'+library.replaceAll('export ','')+'\nconst Modes='+modes+';\n'+(await fs.readFile(path.join(source,'editor.js'),'utf8')).replace(/\}\)\(\);\s*$/,profileUI+'\n})();');
const html=(await fs.readFile(path.join(source,'index.html'),'utf8'))
 .replace('/* EDITOR_STYLE */',await fs.readFile(path.join(source,'editor.css'),'utf8'))
 .replace('/* EDITOR_SCRIPT */',script);
const output=path.resolve(process.argv[2]||path.join(root,'dist/web/save-editor.html'));
await fs.mkdir(path.dirname(output),{recursive:true});await fs.writeFile(output,html);console.log(output);

await fs.writeFile(path.join(path.dirname(output),'save-codec.js'),codec+'\n'+store.replace(/^import .*;\s*/m,'')+'\nexport const modes='+modes+';\n');
await fs.writeFile(path.join(path.dirname(output),'lineup-library.js'),library);
