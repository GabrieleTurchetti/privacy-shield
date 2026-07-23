# Privacy Shield — Hub dashboard (frontend)

A small [Preact](https://preactjs.com/) + [htm](https://github.com/developit/htm)
app that renders the Hub dashboard. It is bundled into **one self-contained
`index.html`** (framework + CSS all inlined — no CDN, works offline) and then
embedded into the firmware as the `DASHBOARD_HTML` string served by
`components/web_dashboard/web_api.c`.

The backend contract is unchanged: the app polls `GET /api/nodes` every 5 s and
POSTs to `/api/node/{mute,unmute,volume}` and `/api/global/{mute,unmute}`.

## Build / update the firmware page

```bash
cd web
npm install        # first time only
npm run build
```

`npm run build`:
1. bundles + minifies `src/app.js` (Preact, htm, app) with esbuild,
2. inlines it and `src/styles.css` into `dist/index.html` (self-contained),
3. writes `dist/preview.html` (same UI with mock data — open it in a browser to
   see the dashboard without a hub),
4. **regenerates the `DASHBOARD_HTML` block inside `web_api.c`** automatically.

After running it, rebuild the firmware (`idf.py build`) and flash the Hub.

## Files

- `src/app.js` — components + polling/state logic
- `src/styles.css` — the design system (dark theme)
- `build.mjs` — bundler + single-file assembler + firmware injector
- `dist/` — generated output (git-ignored)

## Editing

Change `src/app.js` / `src/styles.css`, run `npm run build`, rebuild firmware.
Do **not** hand-edit the `DASHBOARD_HTML` block in `web_api.c` — it is generated.
