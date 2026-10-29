const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const root=path.resolve(__dirname,'../feature/hish_main/src/main/resources/rawfile/term');
function fixture(){
 const frames=[],styles={},events={},resizes=[],optionChanges=[],classes=new Set();let observed;
 const context={console,TextDecoder,Uint8Array,Number,Promise,atob:value=>Buffer.from(value,'base64').toString('latin1'),
  requestAnimationFrame:fn=>{frames.push(fn);return frames.length;},cancelAnimationFrame:()=>{},setTimeout:fn=>{fn();return 1;},
  FitAddon:{FitAddon:class{fit(){resizes.push(true);}}},
  ResizeObserver:class{constructor(fn){observed=fn;}observe(){}disconnect(){}},
  HiSHNativeSelection:{attach:()=>({clear(){}})},
  Terminal:class{constructor(options){this.options=options;this.cols=80;this.rows=24;this.modes={applicationCursorKeysMode:false};this.textarea={setAttribute(){}};}
   open(){}loadAddon(){}onData(){}onResize(){}focus(){}resize(){}write(bytes,done){done();}},
  document:{body:{classList:{add:name=>classes.add(name),toggle(){}},style:{}},
   documentElement:{style:{setProperty:(key,value)=>styles[key]=value}},
   getElementById:()=>({clientWidth:320,clientHeight:640,style:{}}),
   createElement:()=>({getContext:()=>({measureText:()=>({width:100})})})},
  native:{getLigatures:()=>false,getItalic:()=>true,getFontSize:()=>10,getFontFamily:()=> 'Default',
   getCursorBlink:()=>false,getBackgroundColor:()=> '#FFFFFF',onAvailableFontsDetected(){},load:()=>optionChanges.push('load')},
  addEventListener:(key,fn)=>events[key]=fn};
 context.window=context;vm.createContext(context);
 vm.runInContext(fs.readFileSync(path.join(root,'appearance.js'),'utf8'),context);
 vm.runInContext(fs.readFileSync(path.join(root,'term.js'),'utf8'),context);
 function flush(){for(let n=0;frames.length&&n<20;n++){const batch=frames.splice(0);batch.forEach(fn=>fn());}}
 return {context,frames,flush,resizes,optionChanges,styles,events,resize:()=>observed()};
}
test('real terminal startup works without WebGL and starts native exactly once',()=>{
 const f=fixture();f.context.onload();f.flush();assert.deepEqual(f.optionChanges,['load']);
 assert.equal(f.context.term.options.cursorBlink,false);assert.equal(f.context.term.options.fontSize,10);
 assert.equal(f.context.term.options.theme.foreground,'#000000');
});
test('solid backgrounds select readable text, including ArkUI alpha format',()=>{
 const f=fixture(),appearance=f.context.HiSHAppearance;
 assert.equal(appearance.background('#FFFFFF').ink,'#000000');assert.equal(appearance.background('#000000').ink,'#FFFFFF');
 assert.equal(appearance.background('#00FFFFFF').css,'rgb(0,0,0)');assert.equal(appearance.background('#FFFFFFFF').css,'rgb(255,255,255)');
 assert.equal(appearance.background('url(untrusted)'),null);
 for(let r=0;r<256;r+=17)for(let g=0;g<256;g+=17)for(let b=0;b<256;b+=17){
  const color='#'+[r,g,b].map(v=>v.toString(16).padStart(2,'0')).join('');assert(appearance.background(color).contrast>=4.5,color);
 }
});
test('virtual-key/container and window resizing coalesce without relative scroll changes',()=>{
 const f=fixture();f.context.onload();f.flush();const before=f.resizes.length;
 for(let n=0;n<20;n++){f.resize();f.events.resize();}assert.equal(f.frames.length,1);f.flush();assert.equal(f.resizes.length,before+1);
 f.context.exports.setFontSize(NaN);assert.equal(f.context.term.options.fontSize,10);
 f.context.exports.setFontSize(18);f.flush();assert.equal(f.context.term.options.fontSize,18);
});
test('false cursor preference is applied even if an existing terminal has blinking enabled',()=>{
 const f=fixture();f.context.onload();f.flush();f.context.term.options.cursorBlink=true;f.context.syncPrefs();assert.equal(f.context.term.options.cursorBlink,false);
});
