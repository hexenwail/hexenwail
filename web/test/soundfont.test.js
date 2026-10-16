import test from 'node:test';
import assert from 'node:assert/strict';
import { settleSoundFonts, SOUNDFONT_PATH, soundFontKind } from '../lib/soundfont.js';
import { mapImportedPath } from '../lib/paths.js';

// A minimal RIFF "sfbk": an INFO list holding ifil (major, minor), then an
// empty sdta list, laid out as a SoundFont writer lays them out.
function chunk(id, body) {
  const out = new Uint8Array(8 + body.length + (body.length & 1));
  out.set([...id].map((c) => c.charCodeAt(0)), 0);
  new DataView(out.buffer).setUint32(4, body.length, true);
  out.set(body, 8);
  return out;
}
function cat(...parts) {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let o = 0;
  for (const p of parts) { out.set(p, o); o += p.length; }
  return out;
}
function list(type, ...subs) {
  return chunk('LIST', cat(new TextEncoder().encode(type), ...subs));
}
function soundFont(major, { ifil = true } = {}) {
  const ifilChunk = chunk('ifil', new Uint8Array([major, 0, 1, 0]));
  const name = chunk('INAM', new TextEncoder().encode('test\0'));
  const info = ifil ? list('INFO', name, ifilChunk) : list('INFO', name);
  return chunk('RIFF', cat(new TextEncoder().encode('sfbk'), info, list('sdta')));
}

test('soundFontKind reads the ifil major version', () => {
  assert.equal(soundFontKind(soundFont(2)), 'sf2');
  assert.equal(soundFontKind(soundFont(3)), 'sf3');
  // No ifil: a RIFF sfbk is left to the engine to judge.
  assert.equal(soundFontKind(soundFont(2, { ifil: false })), 'sf2');
});

test('soundFontKind refuses what is not a SoundFont, without reading past the end', () => {
  assert.equal(soundFontKind(new Uint8Array()), 'invalid');
  assert.equal(soundFontKind(new TextEncoder().encode('MThd not a soundfont')), 'invalid');
  const wave = soundFont(2);
  wave.set(new TextEncoder().encode('WAVE'), 8);
  assert.equal(soundFontKind(wave), 'invalid');
  assert.equal(soundFontKind('RIFF....sfbk'), 'invalid');
  // Truncated in the middle of the INFO list, and a chunk size far past the
  // end: both stop cleanly.
  assert.equal(soundFontKind(soundFont(3).slice(0, 30)), 'sf2');
  const huge = soundFont(2);
  new DataView(huge.buffer).setUint32(16, 0xfffffff0, true);
  assert.equal(soundFontKind(huge), 'sf2');
});

test('the real TimGM6mb layout reads as sf2 via the importer path', () => {
  assert.equal(mapImportedPath('TimGM6mb.sf2'), SOUNDFONT_PATH);
});

test('settleSoundFonts keeps the last usable SoundFont and says why the others went', () => {
  const pak = { path: 'data1/pak0.pak', bytes: new Uint8Array([1]), source: 'pak0.pak' };
  const a = { path: SOUNDFONT_PATH, bytes: soundFont(2), source: 'A.sf2' };
  const b = { path: SOUNDFONT_PATH, bytes: soundFont(2), source: 'B.sf2' };
  const c = { path: SOUNDFONT_PATH, bytes: soundFont(3), source: 'Fluid.sf2' };
  const d = { path: SOUNDFONT_PATH, bytes: new Uint8Array([0, 1, 2]), source: 'junk.sf2' };

  const one = settleSoundFonts([pak, a]);
  assert.deepEqual(one.items, [pak, a]);
  assert.equal(one.soundFontKept, true);
  assert.deepEqual(one.notes, ['SoundFont: A.sf2, for the MIDI music.']);

  const many = settleSoundFonts([a, pak, c, b, d]);
  assert.deepEqual(many.items, [pak, b]);
  assert.equal(many.soundFontKept, true);
  assert.ok(many.notes.some((n) => n.startsWith('Fluid.sf2 is an SF3')));
  assert.ok(many.notes.some((n) => n.startsWith('junk.sf2 is not a SoundFont')));
  assert.ok(many.notes.some((n) => n.startsWith('A.sf2 was not used') && n.includes('B.sf2')));
  assert.ok(many.notes.includes('SoundFont: B.sf2, for the MIDI music.'));

  const none = settleSoundFonts([pak, c]);
  assert.deepEqual(none.items, [pak]);
  assert.equal(none.soundFontKept, false);
  assert.equal(none.notes.length, 1);

  assert.deepEqual(settleSoundFonts([pak]), { items: [pak], notes: [], soundFontKept: false });
});
