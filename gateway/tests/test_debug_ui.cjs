// Deterministic DOM/WebSocket stubs; no browser or external service required.
const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const script = fs.readFileSync(path.join(__dirname, '../debug.html'), 'utf8').match(/<script>([\s\S]*?)<\/script>/)[1];

function page() {
  class Element {
    constructor() { this.children = []; this.disabled = false; this.checked = false; this.value = ''; this.classList = {toggle() {}, remove() {}, add() {}}; }
    set textContent(value) { this.text = value; this.children = []; }
    get textContent() { return this.text; }
    replaceChildren(...children) { this.children = []; children.forEach(c => this.append(c)); }
    append(child) { child.parent = this; this.children.push(child); }
    prepend(child) { child.parent = this; this.children.unshift(child); }
    add(child) { this.append(child); }
    get lastChild() { return this.children.at(-1); }
    remove() { this.parent.children = this.parent.children.filter(c => c !== this); }
  }
  const elements = new Map();
  const get = id => { if (!elements.has(id)) elements.set(id, new Element()); return elements.get(id); };
  const sent = [], revoked = [], handlers = {};
  class Socket {
    static OPEN = 1;
    readyState = 1;
    send(data) { sent.push(JSON.parse(data)); }
    close() { this.readyState = 3; this.onclose(); }
  }
  const context = vm.createContext({
    document: {getElementById:get, createElement:() => new Element()},
    WebSocket:Socket, location:{protocol:'http:', host:'127.0.0.1:8765'},
    Option:class extends Element { constructor(text, value) { super(); this.textContent = text; this.value = value; } },
    URL:{createObjectURL:() => 'blob:mock-' + Math.random(), revokeObjectURL:url => revoked.push(url)},
    Blob, atob, Date, Uint8Array,
    setTimeout:() => 1, clearTimeout() {},
    window:{addEventListener:(name, callback) => handlers[name] = callback},
  });
  vm.runInContext(script, context);
  const event = payload => message({event:'debug.event', connection_id:'device-1', payload});
  const message = data => vm.runInContext('handle(' + JSON.stringify(data) + ')', context);
  message({event:'debug.status', devices:[{id:'device-1', camera:true, mock:true, state:'connected'}]});
  return {get, sent, revoked, handlers, event, message, context};
}

test('capture requires fresh consent and cancel/clear sends no new capture', () => {
  const p = page();
  assert.equal(p.get('capture').disabled, true);
  p.get('consent').checked = true; p.get('consent').onchange();
  assert.equal(p.get('capture').disabled, false);
  p.get('capture').onclick();
  assert.equal(p.sent.at(-1).event, 'camera.capture');
  assert.equal(p.sent.at(-1).consent, true);
  assert.equal(p.get('consent').checked, false);
  assert.equal(p.get('capture').disabled, true);
  p.get('cancel').onclick();
  assert.equal(p.sent.at(-1).event, 'camera.cancel');
  p.event({event:'camera.capture.error', code:'cancelled'});
  assert.equal(p.get('cancel').disabled, true);
  assert.equal(p.get('capture').disabled, true);
  p.get('clear').onclick();
  assert.deepEqual(p.sent.slice(-2).map(m => m.event), ['camera.cancel','debug.select']);
});

test('text is plain text, deltas concatenate and each turn resets metrics', () => {
  const p = page();
  p.event({event:'asr.final', text:'<script>not executable</script>'});
  assert.equal(p.get('asr').textContent, '<script>not executable</script>');
  p.event({event:'llm.delta', text:'你好'}); p.event({event:'llm.delta', text:'，世界'});
  assert.equal(p.get('llm').textContent, '你好，世界');
  p.event({event:'reply.metrics', timings_ms:{asr:2, llm_first:3, llm:8, tts:9, send:1, total:20}});
  assert.equal(p.get('llmMs').textContent, '8 ms');
  p.event({event:'session.state', state:'listening'});
  assert.equal(p.get('llm').textContent, '—');
  assert.equal(p.get('totalMs').textContent, '—');
});

test('snapshot is memory-only, URLs revoked on reset and decode failures reported', () => {
  const p = page();
  const payload = {event:'camera.snapshot.data', jpeg_base64:'/9h4/9k=', size:5};
  p.event(payload);
  const image = p.get('snapshot').children[0];
  assert.match(image.src, /^blob:/);
  image.onerror();
  assert.match(p.get('error').textContent, /JPEG 解码失败/);
  assert.equal(p.revoked.length, 1);
  p.event(payload);
  p.message({event:'debug.reset', reason:'activity_ended'});
  assert.equal(p.revoked.length, 2);
  assert.equal(p.get('asr').textContent, '—');
  assert.equal(p.get('snapshot').children[0].textContent, '尚未授权拍照');
});

test('foreign devices ignored, stream gaps clear partial text and logs bounded', () => {
  const p = page();
  p.message({event:'debug.event', connection_id:'other', payload:{event:'asr.final', text:'wrong'}});
  assert.notEqual(p.get('asr').textContent, 'wrong');
  for (let i = 0; i < 120; i++) p.event({event:'reply.stage', stage:'llm'});
  assert.equal(p.get('events').children.length, 100);
  p.event({event:'llm.delta', text:'partial'});
  p.message({event:'debug.gap'});
  assert.equal(p.get('llm').textContent, '—');
  assert.match(p.get('error').textContent, /不完整/);
});

test('disconnect, device switch and pagehide purge data and revoke images', () => {
  const p = page();
  p.event({event:'asr.final', text:'private'});
  p.event({event:'camera.snapshot.data', jpeg_base64:'/9h4/9k=', size:5});
  p.handlers.pagehide();
  assert.equal(p.revoked.length, 1);
  assert.equal(p.get('asr').textContent, '—');
  assert.equal(p.get('capture').disabled, true);
});

test('ambient transcript stays separate from directed answers and is cleared on reset', () => {
  const p = page();
  p.event({event:'asr.final', text:'问给助手的问题'});
  p.event({event:'llm.delta', text:'助手回复'});
  p.event({event:'ambient.final', text:'<img>同桌聊天', audio_ms:1200, asr_ms:2000, ambient_segments:1});
  assert.equal(p.get('ambient').textContent, '<img>同桌聊天');
  assert.match(p.get('ambientMeta').textContent, /2000 ms/);
  assert.equal(p.get('asr').textContent, '问给助手的问题');
  assert.equal(p.get('llm').textContent, '助手回复');
  p.event({event:'hint.suggestion', text:'旧话题'});
  p.event({event:'ambient.speech'});
  assert.equal(p.get('hint').textContent, '—');
  p.event({event:'ambient.gap'});
  assert.match(p.get('ambientMeta').textContent, /跳过旧片段/);
  p.message({event:'debug.reset', reason:'activity_ended'});
  assert.equal(p.get('ambient').textContent, '—');
});
