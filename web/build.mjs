// Build: bundle Preact app + CSS into ONE self-contained index.html (no CDN),
// plus a preview.html that renders with mock data, plus the C string for the
// firmware (components/web_dashboard/web_api.c :: DASHBOARD_HTML).
import { build } from 'esbuild';
import { readFileSync, writeFileSync, mkdirSync } from 'fs';

mkdirSync('dist', { recursive: true });

const res = await build({
  entryPoints: ['src/app.js'],
  bundle: true,
  minify: true,
  format: 'iife',
  target: ['es2019'],
  legalComments: 'none',
  write: false,
});
const js = res.outputFiles[0].text.trim();
const css = readFileSync('src/styles.css', 'utf8');

// Runs before first paint so the saved/preferred theme is applied with no flash.
const noFlash =
  '<script>(function(){try{var t=localStorage.getItem("ps-theme");' +
  'if(t!=="light"&&t!=="dark")t=(window.matchMedia&&matchMedia("(prefers-color-scheme: light)").matches)?"light":"dark";' +
  'document.documentElement.dataset.theme=t;}catch(e){}})();</script>';

const page = (mock) =>
  '<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8">' +
  '<meta name="viewport" content="width=device-width,initial-scale=1">' +
  '<meta name="color-scheme" content="dark light">' +
  '<title>Privacy Shield</title>' + noFlash +
  '<style>' + css + '</style></head><body>' +
  '<div id="app"></div>' +
  (mock ? '<script>window.PS_MOCK=true;</script>' : '') +
  '<script>' + js + '</script></body></html>';

const html = page(false);
writeFileSync('dist/index.html', html);
writeFileSync('dist/preview.html', page(true));

// ---- Emit the C string for web_api.c (chunked adjacent literals) ----
// Byte-exact: escape \, ", control chars and every non-ASCII byte as 3-digit
// octal so the literal is valid C regardless of content (CSS newlines, UTF-8…).
const buf = Buffer.from(html, 'utf8');
const CHUNK = 180;
let lines = [];
for (let i = 0; i < buf.length; i += CHUNK) {
  let s = '';
  for (let j = i; j < Math.min(i + CHUNK, buf.length); j++) {
    const b = buf[j];
    if (b === 0x5c) s += '\\\\';
    else if (b === 0x22) s += '\\"';
    else if (b === 0x0a) s += '\\n';
    else if (b === 0x0d) s += '\\r';
    else if (b === 0x09) s += '\\t';
    else if (b >= 0x20 && b <= 0x7e) s += String.fromCharCode(b);
    else s += '\\' + b.toString(8).padStart(3, '0');
  }
  lines.push('"' + s + '"');
}
const cstr =
  '/* AUTO-GENERATED from web/ by `npm run build` — do not edit by hand. */\n' +
  'static const char *DASHBOARD_HTML =\n' + lines.join('\n') + ';\n';
writeFileSync('dist/dashboard_html.c.txt', cstr);

// ---- Inject straight into the firmware (replace the DASHBOARD_HTML block) ----
const API = '../components/web_dashboard/web_api.c';
let src = readFileSync(API, 'utf8');
const a = src.indexOf('static const char *DASHBOARD_HTML');
const b = src.indexOf('esp_err_t dashboard_get_handler', a);
if (a === -1 || b === -1) throw new Error('DASHBOARD_HTML markers not found in web_api.c');
src = src.slice(0, a) + cstr.trim() + '\n\n' + src.slice(b);
writeFileSync(API, src);

console.log('index.html   ', html.length, 'bytes');
console.log('bundle js    ', js.length, 'bytes');
console.log('C literals   ', lines.length, 'chunks');
console.log('injected into', API);
