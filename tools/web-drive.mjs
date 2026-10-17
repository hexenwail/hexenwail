// web-drive.mjs -- walk the web player page through its phases in headless
// Chromium over the DevTools Protocol, screenshotting each one.
//
//   nix shell nixpkgs#chromium nixpkgs#nodejs --command \
//     node tools/web-drive.mjs URL OUTDIR [WIDTH HEIGHT [mobile]]
//
// URL is a served artifact (scripts/wasm-assemble-artifact.sh, then e.g.
// `python3 -m http.server`).  Phases: 01-boot (first visit: the free demo
// downloading), 02-ready (the start prompt), 03-running (after clicking the
// view), then 04-drawer and 05-files on a desktop run.  Each shot logs the
// status line, the boot line, the overlay phase and the engine state;
// OUTDIR/log.txt holds page errors and the runtime log.  `mobile` emulates a
// touch phone and stops after 03-running.  The first visit downloads 13 MB from
// archive.org, so allow it minutes on a slow link.  Unset LD_LIBRARY_PATH on
// this box first, or chromium dies on a glibc mismatch.
import { spawn } from 'node:child_process';
import { writeFileSync, mkdirSync } from 'node:fs';
import { setTimeout as sleep } from 'node:timers/promises';

const [url, out, w = '1280', h = '800', mode = ''] = process.argv.slice(2);
const mobile = mode === 'mobile';
mkdirSync(out, { recursive: true });
const port = 9300 + Math.floor(Math.random() * 500);
const chrome = spawn('chromium', ['--headless=new', '--no-sandbox', `--remote-debugging-port=${port}`,
  `--user-data-dir=${out}/profile`, `--window-size=${w},${h}`, '--use-gl=angle', '--use-angle=swiftshader',
  '--enable-unsafe-swiftshader', '--autoplay-policy=no-user-gesture-required', 'about:blank'],
  { stdio: ['ignore', 'ignore', 'pipe'] });
const log = [];
const say = (m) => { log.push(m); console.log(m); };
let wsUrl;
for (let i = 0; i < 60 && !wsUrl; i++) {
  await sleep(250);
  try {
    const list = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    wsUrl = list.find((t) => t.type === 'page')?.webSocketDebuggerUrl;
  } catch {}
}
const ws = new WebSocket(wsUrl);
await new Promise((r) => ws.addEventListener('open', r));
let id = 0; const pending = new Map();
ws.addEventListener('message', (e) => {
  const msg = JSON.parse(e.data);
  if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id); }
  if (msg.method === 'Runtime.consoleAPICalled') {
    const text = msg.params.args.map((a) => a.value ?? a.description).join(' ');
    if (/error|warn/i.test(msg.params.type)) log.push(`[console.${msg.params.type}] ${text}`);
  }
  if (msg.method === 'Runtime.exceptionThrown') log.push(`[exception] ${msg.params.exceptionDetails.exception?.description ?? msg.params.exceptionDetails.text}`);
});
const send = (method, params = {}) => new Promise((r) => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evaluate = async (expr) => (await send('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true })).result?.result?.value;
const shot = async (name) => {
  const r = await send('Page.captureScreenshot', { format: 'png' });
  writeFileSync(`${out}/${name}.png`, Buffer.from(r.result.data, 'base64'));
  say(`shot ${name}: status="${await evaluate("document.getElementById('status-text')?.textContent")}" boot="${await evaluate("document.getElementById('progress-text')?.textContent")}" phase=${await evaluate("document.getElementById('overlay')?.dataset.phase")} state=${await evaluate('document.body.dataset.engineState')}`);
};
const waitFor = async (expr, ms, label) => {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) { if (await evaluate(expr)) return true; await sleep(250); }
  say(`TIMEOUT waiting for ${label}`); return false;
};
await send('Runtime.enable'); await send('Page.enable');
await send('Emulation.setDeviceMetricsOverride', { width: +w, height: +h, deviceScaleFactor: mobile ? 3 : 1, mobile });
if (mobile) await send('Emulation.setTouchEmulationEnabled', { enabled: true, maxTouchPoints: 5 });
await send('Page.navigate', { url });
await sleep(1500); await shot('01-boot');
await waitFor("document.getElementById('overlay')?.dataset.phase === 'ready'", 480000, 'ready');
await shot('02-ready');
// Click the overlay centre: the start.
const box = await evaluate("(() => { const r = document.getElementById('overlay').getBoundingClientRect(); return [r.x + r.width/2, r.y + r.height/2]; })()");
for (const type of ['mousePressed', 'mouseReleased'])
  await send('Input.dispatchMouseEvent', { type, x: box[0], y: box[1], button: 'left', clickCount: 1 });
await waitFor("document.body.dataset.engineState === 'running'", 20000, 'running');
await sleep(8000); await shot('03-running');
if (mobile) { writeFileSync(`${out}/log.txt`, log.join('\n')); ws.close(); chrome.kill(); process.exit(0); }
say('canvas: ' + await evaluate("(() => { const c = document.getElementById('canvas'); const r = c.getBoundingClientRect(); const w = document.getElementById('wrap').getBoundingClientRect(); return JSON.stringify({style: c.getAttribute('style'), cls: c.className, rect: [r.x,r.y,r.width,r.height], wrap: [w.x,w.y,w.width,w.height], wh: [c.width,c.height], parent: c.parentElement.id, pos: getComputedStyle(c).position}); })()"));
await evaluate("document.getElementById('keys-button').click()"); await sleep(600);
await send('Emulation.setDeviceMetricsOverride', { width: +w, height: 2400, deviceScaleFactor: 1, mobile: false });
await sleep(500); await shot('04-drawer');
await send('Emulation.setDeviceMetricsOverride', { width: +w, height: +h, deviceScaleFactor: 1, mobile: false });
await evaluate("document.getElementById('keys-button').click()");
await evaluate("document.getElementById('files-button').click()"); await sleep(500); await shot('05-files');
writeFileSync(`${out}/log.txt`, log.join('\n') + '\n' + (await evaluate("document.getElementById('runtime-log').textContent")));
ws.close(); chrome.kill();
