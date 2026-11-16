// Headless Move Studio for tools/d1_moves.py: opens <src mods dir> (d1bartz = Dissidia (2008) Bartz in the 012 layout,
// bartz = retail Bartz as Save as mod lays it out), and copies the given D1 commands into bartz with the Studio's own
// copy-a-move code (studio/copy.js), then saves. Needs Chrome / Chromium and the playwright-core package
// (CHROME=<binary>, NODE_PATH pointing at its node_modules).
// node d1_moves_studio.js <studio index.html> <src mods dir> <012 iso> <out dir> <cmd,cmd,...>
const { chromium } = require('playwright-core');
const fs = require('fs'), path = require('path');
const [, , html, srcDir, iso, outDir, cmdList] = process.argv;

function walk(d, base = d, out = {}) {
  for (const n of fs.readdirSync(d)) {
    const p = path.join(d, n);
    if (fs.statSync(p).isDirectory()) walk(p, base, out);
    else out[path.relative(base, p)] = fs.readFileSync(p).toString('base64');
  }
  return out;
}

(async () => {
  const browser = await chromium.launch({ executablePath: process.env.CHROME || '/usr/bin/google-chrome', headless: true, args: ['--allow-file-access-from-files'] });
  const page = await browser.newPage();
  page.on('console', (m) => { const t = m.text(); if (!/THREE|WebGL|GPU stall/.test(t)) console.log('[page]', t.slice(0, 400)); });
  page.on('pageerror', (e) => console.log('[pageerror]', e.message));
  await page.goto('file://' + path.resolve(html));
  await page.waitForFunction(() => typeof window.studioOpenMem === 'function');
  // toasts -> log
  await page.evaluate(() => { const t = window.toast; window.toast = (msg, kind) => { console.log('TOAST ' + (kind || '') + ': ' + msg); try { t(msg, kind); } catch (e) {} }; });
  const files = walk(srcDir);
  await page.evaluate(async (files) => {
    const obj = {};
    for (const [k, b64] of Object.entries(files)) {
      const bin = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
      obj[k] = /\.json$/.test(k) ? new TextDecoder().decode(bin) : bin;
    }
    await window.studioOpenMem(obj);
  }, files);
  await page.setInputFiles('#isoInput', iso);
  await page.waitForFunction(() => S.retailChars && S.retailChars.length > 0 && S.rig, null, { timeout: 120000 });
  const slug = 'bartz';
  await page.evaluate(async () => {
    const mj = JSON.parse(new TextDecoder().decode(await FS.read('bartz/mod.json')));
    mj.move_names = { ...(S.abilities.fiv100 || {}) };
    await FS.write('bartz/mod.json', JSON.stringify(mj, null, 1) + '\n');
    await scanMods(S.modsBase); fillCharPick();
    await pickChar('bartz', 'bartz/models/p_fiv100_1p.gmo');
  });
  console.log('retail mod folder:', slug);
  // the Studio only adds abilities to added characters: open the folder as one for the copy, then mark it retail again
  const res = await page.evaluate(async (cmds) => {
    const out = [];
    S.mod.kind = 'extra';
    S.mod.character.based_on = S.mod.character.code;
    S.mod.character.based_on_id = 9;
    for (const c of cmds) {
      const before = Object.keys(S.mod.move_names).length;
      await cpCopyMove({ mod: 'd1bartz' }, c);
      const keys = Object.keys(S.mod.move_names);
      out.push({ cmd: c, added: keys.length - before, last: S.mod.move_names[keys[keys.length - 1]], notes: CP.notes[Math.max(...Object.keys(CP.notes).map(Number))] });
    }
    S.mod.kind = 'retail';
    delete S.mod.character.based_on; delete S.mod.character.based_on_id;
    await save();
    return out;
  }, cmdList.split(',').map((x) => parseInt(x, 16)));
  console.log(JSON.stringify(res, null, 1));
  const written = await page.evaluate((slug) => {
    const o = {};
    for (const [k, v] of FS.mem) if (k.startsWith(slug + '/')) {
      const u8 = typeof v === 'string' ? new TextEncoder().encode(v) : v;
      let s = ''; for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000));
      o[k] = btoa(s);
    }
    return o;
  }, slug);
  for (const [k, b64] of Object.entries(written)) {
    const p = path.join(outDir, k);
    fs.mkdirSync(path.dirname(p), { recursive: true });
    fs.writeFileSync(p, Buffer.from(b64, 'base64'));
  }
  console.log('wrote', Object.keys(written).length, 'files to', outDir);
  await browser.close();
})().catch((e) => { console.error(e); process.exit(1); });
