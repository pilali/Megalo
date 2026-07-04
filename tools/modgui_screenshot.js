// Render the Megalo / MegaloHN modguis with the real HTML/CSS/JS and take
// the store screenshots at the default port values (knob rotations, LED
// states and the envelope drawing are reproduced exactly as MOD-UI would).
//
// Requirements: node with `playwright` resolvable (a preinstalled Chromium
// works via PLAYWRIGHT_BROWSERS_PATH) and `jquery` installed somewhere
// require() can find it (`npm i jquery` at the repo root is enough).
//
// Run from the repo root after any modgui change, then regenerate the
// thumbnails (165x85 and 330x216) from the screenshots:
//   node tools/modgui_screenshot.js
const fs = require('fs');
const path = require('path');
const { chromium } = require('playwright');

const REPO = process.cwd();
const JQUERY = fs.readFileSync(require.resolve('jquery/dist/jquery.min.js'), 'utf8');

// LV2 defaults (from the .ttl files).
const COMMON_PORTS = {
  onset_threshold: 0.15, sample_ms: 150, attack_skip_ms: 50, blend: 0.8,
  grain_size_ms: 100, grain_xfade_ms: 40, base_pitch: 0,
  pitch1_semi: -12, pitch1_level: 0.5, pitch2_semi: 12, pitch2_level: 0.5,
  detune_cents: 10, chorus_rate: 0.5, detune_blend: 0.3,
  filter_type: 0, filter_cutoff: 3000, filter_q: 0.7,
  env_attack: 100, env_decay: 200, env_sustain: 0.8, env_release: 1000,
  detune_enable: 0, pitch1_enable: 1, pitch2_enable: 0, dry_level: 1,
};
const HN_PORTS = {
  hn_brightness: 0, hn_damping: 0, hn_even_odd: 0, hn_noise: 0.4, hn_width: 0.3,
};

// Knob ranges for the dial rotation (MOD-UI rotates the dial itself;
// mod-widget-rotation="270" → −135°..+135°). filter_cutoff is logarithmic.
const KNOB_RANGES = {
  pitch1_semi: [-24, 24], pitch1_level: [0, 1],
  pitch2_semi: [-24, 24], pitch2_level: [0, 1],
  detune_cents: [0, 50], chorus_rate: [0.1, 8], detune_blend: [0, 1],
  filter_cutoff: [20, 20000, 'log'], filter_q: [0.1, 10],
  base_pitch: [-12, 12], blend: [0, 1], dry_level: [0, 2], onset_threshold: [0, 1],
  hn_brightness: [-1, 1], hn_damping: [0, 1], hn_even_odd: [-1, 1],
  hn_noise: [0, 1], hn_width: [0, 1],
};

function buildPage(bundle) {
  const dir = path.join(REPO, bundle, 'modgui');
  let html = fs.readFileSync(path.join(dir, 'icon-megalo.html'), 'utf8');
  let css = fs.readFileSync(path.join(dir, 'stylesheet-megalo.css'), 'utf8');
  const gui = fs.readFileSync(path.join(dir, 'script-megalo.js'), 'utf8');

  // Strip the mustache bits: template class + the audio I/O jack blocks
  // (they live outside the panel and need MOD-UI to render).
  html = html.replace(/\{\{\{cns\}\}\}/g, '');
  html = html.split('<!-- Audio I/O jacks')[0];
  css = css.replace(/\{\{\{cns\}\}\}/g, '');

  return `<!DOCTYPE html><html><head><meta charset="utf-8">
<style>
  html, body { margin: 0; padding: 0; background: transparent; }
  #stage { position: relative; }
  ${css}
</style></head>
<body><div id="stage">${html}</div>
<script>${JQUERY}<\/script>
<script>var megaloGui = ${gui};<\/script>
</body></html>`;
}

async function render(page, bundle, ports, panelW, panelH, shots) {
  const pageHtml = buildPage(bundle);
  await page.setContent(pageHtml, { waitUntil: 'load' });

  await page.evaluate(({ ports, ranges }) => {
    const icon = $('.megalo-panel');
    const portList = Object.keys(ports).map(s => ({ symbol: s, value: ports[s] }));
    // Drive the real modgui script exactly like MOD-UI does on 'start'.
    megaloGui({ type: 'start', icon: icon, data: {}, ports: portList },
              { set_port_value: function () {} });

    // What MOD-UI core normally does and the script does not:
    // dial rotation, switch LEDs, bypass light.
    for (const sym in ranges) {
      const [lo, hi, scale] = ranges[sym];
      const v = ports[sym];
      if (v === undefined) continue;
      const n = scale === 'log'
        ? Math.log(v / lo) / Math.log(hi / lo)
        : (v - lo) / (hi - lo);
      const deg = (n - 0.5) * 270;
      icon.find('.megalo-knob-rotor[mod-port-symbol="' + sym + '"]')
          .css('transform', 'rotate(' + deg.toFixed(1) + 'deg)');
    }
    for (const sym of ['pitch1_enable', 'pitch2_enable', 'detune_enable'])
      icon.find('.megalo-led[mod-port-symbol="' + sym + '"]')
          .toggleClass('on', ports[sym] >= 0.5);
    icon.find('.megalo-logo-light').addClass('on');
  }, { ports, ranges: KNOB_RANGES });

  await page.waitForTimeout(150);
  const panel = page.locator('.megalo-panel');
  for (const { file, dsf } of shots) {
    await page.setViewportSize({ width: panelW, height: panelH });
    const buf = await panel.screenshot({ omitBackground: true });
    fs.writeFileSync(file, buf);
  }
}

(async () => {
  const browser = await chromium.launch();
  const out = [];
  for (const spec of [
    { bundle: 'megalo.lv2', ports: COMMON_PORTS, w: 720, h: 470, dsf: 1 },
    { bundle: 'megaloHN.lv2', ports: { ...COMMON_PORTS, ...HN_PORTS }, w: 720, h: 470, dsf: 2 },
  ]) {
    const ctx = await browser.newContext({
      viewport: { width: spec.w + 40, height: spec.h + 40 },
      deviceScaleFactor: spec.dsf,
    });
    const page = await ctx.newPage();
    page.on('pageerror', e => console.error(spec.bundle, 'pageerror:', e.message));
    page.on('console', m => { if (m.type() === 'error') console.error(spec.bundle, 'console:', m.text()); });
    const shot = path.join(REPO, spec.bundle, 'modgui', 'screenshot-megalo.png');
    await render(page, spec.bundle, spec.ports, spec.w, spec.h, [{ file: shot }]);
    out.push(shot);
    await ctx.close();
  }
  await browser.close();
  console.log('rendered:', out.join(' '));
})();
