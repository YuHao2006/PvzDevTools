import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const source=path.join(root,'saveEditor');
const html=(await fs.readFile(path.join(source,'index.html'),'utf8'))
 .replace('/* EDITOR_STYLE */',await fs.readFile(path.join(source,'editor.css'),'utf8'))
 .replace('/* EDITOR_SCRIPT */',await fs.readFile(path.join(source,'editor.js'),'utf8'));
const output=path.resolve(process.argv[2]||path.join(root,'dist/web/save-editor.html'));
await fs.mkdir(path.dirname(output),{recursive:true});await fs.writeFile(output,html);console.log(output);
