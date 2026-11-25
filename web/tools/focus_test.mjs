// Tests the guest-memory snapshot without starting the game or touching its save data.
import assert from 'node:assert/strict';
import * as esbuild from 'esbuild';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const result = await esbuild.build({ entryPoints: [path.join(root, 'web/src/focus-page.ts')], bundle: true, format: 'esm', platform: 'node', write: false });
const { readFocusPage, readRaceFocusPage, moveFocus, focusActivationPoint } = await import('data:text/javascript;base64,' + Buffer.from(result.outputFiles[0].contents).toString('base64'));
const tabs=[{id:1,kind:'tab',x:100,y:0,width:100,height:50},{id:2,kind:'tab',x:160,y:0,width:100,height:50}];
assert.ok(focusActivationPoint({window:1,items:tabs},{...tabs[1]}).x>=200,'overlapping tabs use an unobstructed hit point');
const memory = new ArrayBuffer(0x700000), view = new DataView(memory);
const put = (p, value) => view.setUint32(p, value, true);
const fe=0x10000, layers=0x11000, slots=0x12000, layer=0x13000, win=0x14000, origin=0x15000, vector=0x16000, children=0x17000;
put(0x65b928,fe); put(fe+0xd8,layers); put(layers,slots); put(layers+4,slots+4); put(slots,layer); put(layer,win); put(win+0xe0,origin); put(win+0x10c,vector); put(vector,children); put(vector+4,children+12);
for (let n=0; n<3; n++) {
  const obj=0x20000+n*0x1000, pos=0x30000+n*0x1000;
  put(children+4*n,obj); put(obj,n===2 ? 0x5b611c : 0x5b5d10); put(obj+0xe0,pos);
  put(pos+0x38,20); put(pos+0x3c,20+n*30); put(obj+0x100,100); put(obj+0x104,20);
  if (n===1) put(obj+0x10c,0x10);
}
let page=readFocusPage(memory);
assert.equal(page.items.length,2, 'disabled control is excluded');
assert.equal(page.items[1].kind,'list', 'list control is recognised');
assert.equal(moveFocus(page,page.items[0].id,'down').id,page.items[1].id);
assert.equal(moveFocus(page,page.items[0].id,'up'),undefined);
const bar=0x40000, barOrigin=0x41000, barVector=0x42000, barChildren=0x43000, back=0x44000, backPos=0x45000, callback=0x46000, callbackText=0x47000;
put(fe+0xdc,bar); put(bar+0xe0,barOrigin); put(bar+0x10c,barVector);
put(barVector,barChildren); put(barVector+4,barChildren+4); put(barChildren,back);
put(back,0x5b5d80); put(back+0xe0,backPos); put(backPos+0x38,5); put(backPos+0x3c,452); put(back+0x100,57); put(back+0x104,24);
put(back+0x148,callback); put(callback+0x3c,callbackText); put(callback+0x40,callbackText+6);
new Uint8Array(memory,callbackText,6).set(Buffer.from('GOBACK'));
page=readFocusPage(memory);
assert.equal(page.items.at(-1).command,'GOBACK','enabled bottom bar button exposes its callback');
put(back+0x10c,0x10); assert.equal(readFocusPage(memory).items.length,2,'disabled bottom bar button is excluded');
const combo=0x21000, slider=0x50000;
put(combo,0x5b5c24); put(combo+0x10c,0x40); put(combo+0x1b8,slider); put(combo+0x1c4,160); put(slider,0x5b6a64); view.setUint8(combo+0x1dc,1);
for (const [offset,value] of [[0x11c,472],[0x120,212],[0x124,16],[0x128,13],[0x12c,472],[0x130,65],[0x134,16],[0x138,13]]) put(slider+offset,value);
page=readFocusPage(memory);
assert.deepEqual(page.items[1].scrollDown,{x:472,y:212,width:16,height:13},'expanded combo exposes its scrollbar arrow');
put(slider,0); assert.equal(readFocusPage(memory).items[1].scrollDown,undefined,'unknown slider class is rejected');
put(slider,0x5b6a64); put(combo,0x5b5ba8); view.setUint8(0x65b710,1);
for (const [base,rect] of [[0x224,[107,110,257,205]],[0x234,[107,95,222,103]],[0x274,[106,211,258,221]],[0x254,[106,226,179,244]],[0x264,[185,226,258,244]]]) rect.forEach((v,n)=>put(combo+base+n*4,v));
for (const offset of [0x288,0x28c,0x290]) view.setFloat32(combo+offset,0.5,true);
page=readFocusPage(memory); assert.equal(page.items[1].customEditor,true,'custom colour editor is identified');
assert.deepEqual(page.items[1].customRects[0],{x:107,y:110,width:150,height:95},'custom editor uses the live hit rectangles');
assert.deepEqual(page.items[1].customHsv,[0.5,0.5,0.5]);
assert.equal(page.items[1].scrollDown,undefined,'colour editor does not expose list navigation');
view.setUint8(0x65b710,0);
const sliderAccessor=0x57000;
put(combo,0x5b5e60); put(combo+0x10c,0); put(combo+0x150,1); put(combo+0x154,sliderAccessor);
put(sliderAccessor,0x5b502c); put(sliderAccessor+0x14,128); put(sliderAccessor+0x18,64);
page=readFocusPage(memory); assert.equal(page.items[1].sliderValue,64); assert.equal(page.items[1].sliderCount,128);
put(combo+0x150,0); assert.equal(readFocusPage(memory).items.some(x=>x.id===combo),false,'read-only indicator is excluded');
put(combo,0x5b5d10); put(combo+0x10c,0x10);
const childWindow=0x51000, childOrigin=0x52000, childVector=0x53000, childArray=0x54000, childButton=0x55000, childPos=0x56000;
put(combo,0x5b6358); put(combo+0x10c,0); put(combo+0x14c,childWindow);
put(childWindow+0xe0,childOrigin); put(childOrigin+0x38,3); put(childOrigin+0x3c,4);
put(childWindow+0x10c,childVector); put(childVector,childArray); put(childVector+4,childArray+4);
put(childArray,childButton); put(childButton,0x5b5d10); put(childButton+0xe0,childPos);
put(childPos+0x38,20); put(childPos+0x3c,30); put(childButton+0x100,50); put(childButton+0x104,20);
page=readFocusPage(memory); const nested=page.items.find(x=>x.id===childButton);
assert.deepEqual([nested.x,nested.y,nested.width,nested.height],[43,84,50,20],'nested panel positions compose');
put(combo+0x10c,0x20); assert.equal(readFocusPage(memory).items.some(x=>x.id===childButton),false,'hidden panel children are excluded');
put(combo,0x5b5d10); put(combo+0x10c,0x10);
put(layer+4,1); assert.equal(readFocusPage(memory),null,'pending transition is excluded');
put(layer+4,0); put(vector+4,children+4097); assert.equal(readFocusPage(memory),null,'invalid vector is rejected');
const raceVector=0x58000, raceEntry=0x59000, racePage=0x5a000, raceButton=0x5b000;
put(0x60c9c4,raceVector); put(raceVector,raceEntry); put(raceVector+4,raceEntry+8); put(raceEntry,racePage);
put(0x619784,1280); put(0x619788,960); put(racePage,0x5b2d54); put(racePage+0xc,3); put(racePage+0x120,raceButton);
put(raceButton,0x5b2904); put(raceButton+4,400); put(raceButton+8,300); put(raceButton+12,200); put(raceButton+16,80); view.setUint8(raceButton+0x103,1);
const race=readRaceFocusPage(memory); assert.deepEqual([race.items[0].x,race.items[0].y,race.items[0].width,race.items[0].height],[200,150,100,40]);
assert.equal(race.items[0].command,'GOBACK');
put(racePage,0x5b2b0c); put(racePage+0x120,0); put(racePage+0x128,raceButton);
assert.equal(readRaceFocusPage(memory).items[0].command,'GOBACK','race exit dialog maps its cancel button');
put(racePage,0x5b2d54); put(racePage+0x120,raceButton); put(racePage+0x128,0);
// KB2: the controller (00425EF0) asks the first page of the stack; a second one (the pause's yes/no) does not hide it
put(raceVector+4,raceEntry+16); assert.equal(readRaceFocusPage(memory).items[0].id,raceButton,'the first page of a stack is read'); put(raceVector+4,raceEntry+8);
// result pages (HRaceSummary) have no buttons: Space closes them; under them and after them the replay bar's 7 slots
const replay=0x5c000;
put(racePage,0x5b2e60); put(racePage+0x120,0);
let results=readRaceFocusPage(memory); assert.equal(results.spaceCloses,true,'result pages close on Space'); assert.equal(results.items.length,0);
put(0x60ca58,replay); put(0x657424,2); put(0x5b2ef0,21); put(0x5b2ef4,18);
for(let k=0;k<7;k++){put(replay+0x12c+8*k,314+51*k); put(replay+0x130+8*k,686);}
results=readRaceFocusPage(memory); assert.equal(results.items.length,7,'the replay bar is reachable under the results');
assert.deepEqual([results.items[0].x,results.items[0].y,results.items[0].width,results.items[0].height],[157,343,10.5,9]);
put(raceVector+4,raceEntry); results=readRaceFocusPage(memory);
assert.equal(results.window,replay,'the replay bar alone after the results'); assert.equal(results.items.length,7);
put(0x657424,0); assert.equal(readRaceFocusPage(memory),null,'no page and no race end: nothing'); put(raceVector+4,raceEntry+8);
// the network race's leave notice (HInfo, game type 3): two buttons of the focus layer's own, Space back, Esc out
put(racePage,0x5b2bfc); put(0x6573e8,3);
const notice=readRaceFocusPage(memory);
assert.deepEqual(notice.items.map(x=>x.key),['Space','Escape'],'the leave notice gets back and leave buttons');
assert.equal(notice.padBack,'Space','the pad B goes back to the race there');
assert.ok(notice.items.every(x=>x.label&&x.x>=0&&x.x+x.width<=640&&x.y>=0&&x.y+x.height<=480));
put(0x6573e8,0); assert.equal(readRaceFocusPage(memory).items.length,0,'single-player info pages keep the game keys');
put(racePage,0x5b2d54); put(racePage+0x120,raceButton); put(0x60ca58,0);
// the frontend's message box (FEDialog list [0069A05C]) wins over the layer under it; its second button goes back
put(vector+4,children+12);
const dialogs=0x5d000, dialog=0x5d100, dlgChildren=0x5d400, yes=0x5d500, no=0x5d600;
put(0x69a05c,dialogs); put(dialogs,dialogs+0x10); put(dialogs+4,dialogs+0x14); put(dialogs+0x10,dialog);
put(dialog+0xd4,dlgChildren); put(dlgChildren,dlgChildren+0x10); put(dlgChildren+4,dlgChildren+0x18); put(dlgChildren+0x10,yes); put(dlgChildren+0x14,no);
for(const [b,dy] of [[yes,34],[no,55]]){put(b,0x5ba108); put(b+0xd4,120); put(b+0xd8,dy); put(b+0xdc,160); put(b+0xe0,202); view.setFloat32(b+0xe4,80,true); view.setFloat32(b+0xe8,18,true);}
assert.equal(readFocusPage(memory).window,win,'a hidden dialog leaves the page alone');
view.setUint8(dialog+0xd8,1); put(dialog+0xdc,0xff);
const box=readFocusPage(memory);
assert.equal(box.window,dialog,'an open dialog takes the focus'); assert.deepEqual(box.items.map(x=>[x.x,x.y,x.width,x.height,x.command]),[[280,236,80,18,undefined],[280,257,80,18,'GOBACK']]);
put(dialog+0xdc,0x80); assert.equal(readFocusPage(memory).window,win,'a dialog fading in is not up yet'); put(dialog+0xdc,0); view.setUint8(dialog+0xd8,0);
console.log('focus snapshot and navigation checks passed');

// Exercise actual input routing against a shared guest-memory fixture.
put(vector+4,children+12);
const shared = new SharedArrayBuffer(memory.byteLength);
new Uint8Array(shared).set(new Uint8Array(memory));
const sv = new DataView(shared), mail=0x680000;
const handlers = new Map(), canvasHandlers = new Map(), overlay = { style: {} };
globalThis.window = globalThis;
globalThis.addEventListener = (name, fn) => handlers.set(name, fn);
globalThis.requestAnimationFrame = () => 1;
globalThis.location = { search: '?focus=1' };
// the first element is the focus outline; the rest (own buttons, the pad keyboard) are inert stand-ins
const element = () => ({ style: {}, children: [], appendChild(c){ this.children.push(c); }, replaceChildren(...c){ this.children = c; }, addEventListener(){}, textContent: '' });
let elements = 0;
globalThis.document = { createElement: () => elements++ ? element() : overlay, body: { appendChild() {} }, hasFocus: () => true };
const canvas = { style: {}, addEventListener(name,fn){ canvasHandlers.set(name,fn); }, getBoundingClientRect(){ return { left:0,top:0,width:640,height:480 }; }, focus(){} };
const inputBundle = await esbuild.build({ entryPoints: [path.join(root,'web/src/input.ts')], bundle: true, format:'esm', platform:'node', write:false });
const { attachInput } = await import('data:text/javascript;base64,' + Buffer.from(inputBundle.outputFiles[0].contents).toString('base64'));
attachInput({ buffer: shared }, mail, canvas);
const key = code => {
  const event = { code, key:'', shiftKey:false, ctrlKey:false, altKey:false, repeat:false, preventDefault(){} };
  handlers.get('keydown')(event); handlers.get('keyup')(event);
};
const messageTypes = () => {
  const count=sv.getInt32(mail+8,true);
  return Array.from({length:count},(_,i)=>sv.getUint32(mail+28+12*(i&255),true));
};
key('ArrowDown'); // First enabled button.
key('ArrowDown'); // List, skipping the disabled button.
assert.equal(overlay.style.top,'80px','focus moved onto the list');
key('Enter');
assert.equal(messageTypes().at(-1),0x200,'entering list moves the game cursor');
await new Promise(resolve=>setTimeout(resolve,500)); // the row seek waits for the game (no game here: its time limit)
sv.setUint8(0x22000+0x1a4,1); // The game reports a hovered row.
key('Enter');
await new Promise(resolve=>setTimeout(resolve,150));
assert.deepEqual(messageTypes().slice(-2),[0x201,0x202],'list confirmation produces one mouse click');
handlers.get('blur')();
sv.setUint32(combo,0x5b5ba8,true); sv.setUint32(combo+0x10c,0x40,true); sv.setUint8(0x65b710,1);
key('Enter'); // Enter editing on the colour plane.
key('ArrowRight');
await new Promise(resolve=>setTimeout(resolve,250));
assert.deepEqual(messageTypes().slice(-4),[0x200,0x201,0x200,0x202],'colour adjustment sends a complete drag');
handlers.get('blur')(); sv.setUint8(0x65b710,0); sv.setUint32(combo,0x5b5e60,true); sv.setUint32(combo+0x10c,0,true);
sv.setUint32(combo+0x150,1,true); sv.setUint32(combo+0x154,sliderAccessor,true);
key('ArrowDown'); key('ArrowDown'); key('Enter'); key('ArrowLeft');
await new Promise(resolve=>setTimeout(resolve,250));
assert.deepEqual(messageTypes().slice(-3),[0x200,0x201,0x202],'slider step sends one click');
handlers.get('blur')(); sv.setInt32(mail+7728,1,true);
key('ArrowDown'); key('Escape');
await new Promise(resolve=>setTimeout(resolve,250));
assert.deepEqual(messageTypes().slice(-3),[0x200,0x201,0x202],'race pause Escape activates its continue button');
handlers.get('blur')(); sv.setInt32(mail+7728,0,true); sv.setUint32(combo,0x5bac84,true); sv.setUint32(combo+0x10c,0x40,true);
key('ArrowLeft');
assert.deepEqual(messageTypes().slice(-2),[0x100,0x101],'binding capture forwards the key to the game');
handlers.get('blur')(); sv.setUint32(combo+0x10c,0x10,true);
console.log('focus input routing checks passed');
