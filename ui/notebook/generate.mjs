#!/usr/bin/env node
// CLI wrapper around ./render.mjs. Reads the template file and the entry
// payload from disk (or stdin), calls the pure renderer, writes the .wl.
//
// Usage:
//   node generate.mjs [--offline] <entry.json|-> [recordIndex] [out.wl]
//
// Output is a Mathematica .wl file. Mathematica opens .wl files natively as
// notebooks via File > Open — the (* ::Style:: *) markers guide cell styling
// and raw code blocks become real evaluatable Input cells (Mathematica does
// the parsing at open time, producing proper BoxData).

import { readFile, writeFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { dirname, join, resolve } from 'node:path';

import { renderNotebook, buildTokens, renderTemplate } from './render.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const TEMPLATE_PATH = join(__dirname, 'template.wl');

// Re-export the pure pieces so downstream Node callers can still import from
// generate.mjs if they were doing so.
export { renderNotebook, buildTokens, renderTemplate };

async function readEntry(entryPath) {
  if (entryPath === '-') {
    const chunks = [];
    for await (const chunk of process.stdin) chunks.push(chunk);
    return JSON.parse(Buffer.concat(chunks).toString('utf8'));
  }
  return JSON.parse(await readFile(entryPath, 'utf8'));
}

export async function renderFromPayload(entry, recordIdx = 0, opts = {}) {
  const template = await readFile(TEMPLATE_PATH, 'utf8');
  return renderNotebook(template, entry, recordIdx, opts);
}

export async function render(entryPath, recordIdx = 0, opts = {}) {
  const entry = await readEntry(entryPath);
  return renderFromPayload(entry, recordIdx, opts);
}

const isMain = resolve(process.argv[1] || '') === fileURLToPath(import.meta.url);
if (isMain) {
  const args = process.argv.slice(2);
  const offline = args.includes('--offline');
  const positional = args.filter(a => !a.startsWith('--'));
  const [entryArg, idxArg, outArg] = positional;
  if (!entryArg) {
    console.error('usage: node generate.mjs [--offline] <entry.json|-> [recordIndex] [out.wl]');
    console.error('  "-"       read a JSON payload from stdin (case i, user-drawn)');
    console.error('  --offline skip INSPIRE BibTeX fetch');
    console.error('  Minimal stdin payload: {"edges":"{{{1,2},m[1]}}","nodes":"{}"}');
    process.exit(1);
  }
  const idx = idxArg ? Number(idxArg) : 0;
  const rendered = await render(entryArg, idx, { offline });
  if (outArg) {
    await writeFile(outArg, rendered);
    console.error(`wrote ${outArg}`);
  } else {
    process.stdout.write(rendered);
  }
}
