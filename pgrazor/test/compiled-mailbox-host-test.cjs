const assert=require('node:assert/strict'),vm=require('node:vm'),fs=require('node:fs');
const source=fs.readFileSync(__dirname+'/../embedded-agent.js','utf8');
const start=source.indexOf("                if(bootParameters.translatedRole==='host')");
const end=source.indexOf('                // Game service class initialization',start);
assert.ok(start>=0&&end>start);
const code=source.slice(start,end);
function run({selected=true,role='host',fail=false}={}) {
 const calls=[],helper={start(){calls.push('start');if(fail)throw Error('factory-start-failed');}};
 const c={bootParameters:{translatedRole:role,mailboxPath:'/private/mailbox',mailboxSession:'session'},
  PGRazorNativeCorePath:selected?'core':'',translatedBridge:null,translatedTimer:null,
  Java:{use(name){calls.push(name);return {create(path,id){assert.equal(path,'/private/mailbox');assert.equal(id,'session');return helper;}}},performNow(f){f();}},
  control:{event(name,detail){calls.push(name);assert.equal(detail,'Compiled Java mailbox v1');}},
  PGRazorTranslated:{files(){calls.push('legacy-files');return {};},host(){calls.push('legacy-host');return {tick(){calls.push('legacy-tick');}};}},
  setInterval(){calls.push('legacy-timer');return 1;}};
 return {calls,c,execute(){vm.runInNewContext(code,c);}};
}
let h=run();h.execute();assert.deepEqual(h.calls,['dev.pgrazor.RuntimeMailbox','start','translated-host-ready']);assert.equal(h.c.translatedTimer,null);
h=run({fail:true});assert.throws(()=>h.execute(),/factory-start-failed/);assert.deepEqual(h.calls,['dev.pgrazor.RuntimeMailbox','start']);
h=run({selected:false});h.execute();assert.deepEqual(h.calls,['legacy-files','legacy-host','legacy-tick','legacy-timer']);
h=run({role:'native'});h.execute();assert.deepEqual(h.calls,[]);
console.log('PASS compiled mailbox host: Java owns timer, selected failure never falls back, guest untouched');
