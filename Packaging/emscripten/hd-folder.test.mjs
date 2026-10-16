import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';
import vm from 'node:vm';

const source = await readFile(new URL('./hd-folder.js', import.meta.url), 'utf8');
function harness({ picker, remember = true, started = false } = {}) {
 const elements = new Map(['hdChooseBtn','hdForgetBtn','hdStatus'].map(id => [id,{hidden:id==='hdForgetBtn',textContent:id==='hdStatus'?'No HD folder selected.':'',listeners:{},addEventListener(t,f){this.listeners[t]=f;}}]));
 const inputs=[];
 const window = {fetch:async()=>new Response('remote'),Module:{calledRun:true},launch:{started}};
 if(remember) window.showDirectoryPicker=picker;
 const context=vm.createContext({window,Module:window.Module,console,URL,Response,location:{href:'https://preview.test/play/index.html',origin:'https://preview.test'},history:{state:null,replaceState(){}},
  // A browser storage operation that has not finished must not delay activation.
  indexedDB:{open(){return {}; }},
  document:{getElementById:id=>elements.get(id),createElement(){const input={files:[],listeners:{},addEventListener(t,f){this.listeners[t]=f;},click(){},remove(){}};inputs.push(input);return input;},body:{appendChild(){}}},
 });
 vm.runInContext(source,context);
 return {elements,inputs,window,click:()=>elements.get('hdChooseBtn').listeners.click(),status:()=>elements.get('hdStatus').textContent};
}
const folder={name:'hd',async getFileHandle(name){if(name!=='sprites.txt')throw new Error('missing');return {getFile:async()=>new Blob(['index'])};}};

test('selected folder activates before a pending IndexedDB write',async()=>{
 const h=harness({picker:async()=>folder});
 h.click();
 await new Promise(setImmediate);
 assert.match(h.status(),/^Using HD folder "hd"/);
 assert.doesNotMatch(h.status(),/Reload/,'runtime initialization does not mean a game has started');
 assert.equal(h.elements.get('hdForgetBtn').hidden,false);
 assert.equal((await h.window.fetch('https://preview.test/play/hd/sprites.txt')).status,200);
});

test('non-cancellation picker errors are visible',async()=>{
 const h=harness({picker:async()=>{throw Object.assign(new Error('Access denied'),{name:'NotAllowedError'});}});
 await h.click();
 assert.match(h.status(),/Access denied/);
});

test('folder input activates local files and reports a late selection',async()=>{
 const h=harness({remember:false,started:true});
 await h.click();
 const input=h.inputs[0]; input.files=[Object.assign(new Blob(['index']),{webkitRelativePath:'pack/hd/sprites.txt'})];
 await input.listeners.change();
 assert.match(h.status(),/^Using HD folder "hd"/);
 assert.match(h.status(),/Reload/);
 assert.equal(await (await h.window.fetch('https://preview.test/play/hd/sprites.txt')).text(),'index');
});

test('canceling the native picker preserves selection status',async()=>{
 const h=harness({picker:async()=>{throw Object.assign(new Error('Canceled'),{name:'AbortError'});}});
 await h.click();
 assert.equal(h.status(),'No HD folder selected.');
});

test('a failed native picker offers a folder input on the next click',async()=>{
 const h=harness({picker:async()=>{throw new Error('Unavailable');}});
 await h.click();
 await h.click();
 assert.equal(h.inputs.length,1);
 h.inputs[0].files=[];
 await h.inputs[0].listeners.change();
 assert.match(h.status(),/No files were returned/);
});
