// Loaded first in the page by tests/web/smoke.py: counts the web hooks, plays tests/web/0.txt
// and posts what it saw to /result, where smoke.py checks it.
const log = [], errors = [];
let phase = 'start';
const str = a => (a instanceof Uint8Array ? String.fromCharCode(...a) : a);
for (const n of ['consText', 'consChoice', 'consSprites', 'consBacklog', 'consOverlay'])
  self[n] = (...a) => log.push({ phase, hook: n, text: str(a[0]), arg: a[1], y: a[2], size: a[3], off: a[5],
                                 boxes: (a.find(x => x instanceof Int32Array) || []).length / 4 });
const cerr = console.error.bind(console);
console.error = (...a) => {
  const s = a.join(' ');
  if (!/can't find file \[\w*cur\w*\.bmp\]/.test(s)) errors.push('console.error: ' + s); // no cursor pictures in the test game
  cerr(...a);
};
addEventListener('error', e => errors.push('error: ' + e.message));
addEventListener('unhandledrejection', e => errors.push('rejection: ' + e.reason));

const wait = ms => new Promise(r => setTimeout(r, ms));
const until = async (f, ms = 20000) => { for (let t = 0; t < ms && !f(); t += 100) await wait(100); };
const seen = (hook, text) => log.some(r => r.phase === phase && r.hook === hook && r.text.includes(text));
function press(keyCode, code) {
  const ev = type => document.getElementById('canvas').dispatchEvent(
    new KeyboardEvent(type, { bubbles: true, cancelable: true, keyCode, which: keyCode, key: code, code }));
  ev('keydown'); setTimeout(() => ev('keyup'), 50);
}
const enter = () => press(13, 'Enter');
function click(choice) { // the middle of the choice's first character, from consChoice's x, y, size
  const c = document.getElementById('canvas'), r = c.getBoundingClientRect();
  const [x, y, size] = [choice.arg, choice.y, choice.size].map(Number);
  const opt = { bubbles: true, cancelable: true, button: 0,
    clientX: r.left + (x + size / 2) * r.width / c.width, clientY: r.top + (y + size / 2) * r.height / c.height };
  c.dispatchEvent(new MouseEvent('mousemove', opt));
  setTimeout(() => c.dispatchEvent(new MouseEvent('mousedown', opt)), 200);
  setTimeout(() => c.dispatchEvent(new MouseEvent('mouseup', opt)), 300);
}

(async () => {
  await until(() => seen('consText', 'Page one.'), 60000); // the wasm load and start
  let kidoku = 0; // kidoku.dat in the save folder: the second start read the first one's
  try { kidoku = g_cons_module.FS.stat(g_cons_index.savedir + '/kidoku.dat').size; } catch (e) {}
  phase = 'off';
  g_cons_module.consNoTextOverlay = 1;
  enter(); await wait(2000); // page two, no hooks
  phase = 'resend';
  g_cons_module.consNoTextOverlay = 0;
  g_cons_module._consResendText();
  await until(() => seen('consText', 'Page two.') && seen('consSprites', 'SPRITE'), 5000);
  phase = 'choice';
  enter(); await until(() => seen('consChoice', 'Gone'), 5000);
  phase = 'disabled'; // seldisable's option: a click on it does nothing
  click(log.find(r => r.phase === 'choice' && r.hook === 'consChoice' && r.text.includes('Gone')));
  await wait(1500);
  phase = 'picked';
  click(log.find(r => r.phase === 'choice' && r.hook === 'consChoice' && r.text.includes('Left')));
  await until(() => seen('consText', 'Page three.'), 5000);
  await wait(500);
  const FS = g_cons_module.FS, envdata = () => String(FS.readdir('/cons').slice(2).some(d => FS.readdir(`/cons/${d}`).includes('envdata')));
  log.push({ phase: 'persist', hook: 'envdata', text: envdata() }); // written only on quit before
  g_cons_module._consPersist();
  log.push({ phase: 'persist', hook: 'envdata', text: envdata() });
  fetch('/result', { method: 'POST', body: JSON.stringify({ log, errors, kidoku }) });
})().catch(e => fetch('/result', { method: 'POST', body: JSON.stringify({ log, errors: errors.concat('smoke.js: ' + e) }) }));
