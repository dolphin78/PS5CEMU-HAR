// Renders every mockup in this folder to a 1920x1080 JPEG in the folder above (../<name>.jpg).
//
//   node docs/ui-redesign/mockups/render.mjs [name ...]
//
// Needs Playwright with a Chromium (PLAYWRIGHT_BROWSERS_PATH or its own download). Optional: the JPEGs
// are committed, so only someone changing a mockup needs this.

import { createRequire } from 'node:module';
import { readdirSync } from 'node:fs';
import { dirname, join, basename } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

// require() honours NODE_PATH, so a global Playwright works too (NODE_PATH=$(npm root -g))
const { chromium } = createRequire(import.meta.url)('playwright');
const here = dirname(fileURLToPath(import.meta.url));
const wanted = process.argv.slice(2);
const pages = readdirSync(here).filter((f) => f.endsWith('.html') && (!wanted.length || wanted.includes(basename(f, '.html'))));

const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 1920, height: 1080 }, deviceScaleFactor: 1 });
for (const file of pages.sort()) {
	await page.goto(pathToFileURL(join(here, file)).href);
	await page.evaluate(() => document.fonts.ready);
	await page.waitForTimeout(150);
	const out = join(here, '..', basename(file, '.html') + '.jpg');
	await page.screenshot({ path: out, type: 'jpeg', quality: 88 });
	console.log(out);
}
await browser.close();
