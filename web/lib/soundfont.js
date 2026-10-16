/*
 * The player's General MIDI SoundFont (GitHub #114).
 *
 * The web build's MIDI synth is libTiMidity, which reads SF2 and cannot decode
 * SF3 (Vorbis-compressed samples).  The importer stores any .sf2 as
 * data1/soundfont.sf2, the spot SF_FindSoundFont (engine/h2shared/soundfont.c)
 * can reach on the web -- so the extension is all it has seen.  An SF3 renamed
 * to .sf2 is common (FluidR3 circulates that way), and the engine skips one
 * with a developer-only message, which would leave a player with silent music
 * and no reason.  So the file is read here, at import, and refused out loud.
 */

export const SOUNDFONT_PATH = 'data1/soundfont.sf2';

function fourcc(bytes, offset) {
  return String.fromCharCode(bytes[offset], bytes[offset + 1], bytes[offset + 2], bytes[offset + 3]);
}

function u32(bytes, offset) {
  return (bytes[offset] | (bytes[offset + 1] << 8) | (bytes[offset + 2] << 16) | (bytes[offset + 3] << 24)) >>> 0;
}

/*
 * 'sf2', 'sf3', or 'invalid' (not a RIFF "sfbk" form at all).
 *
 * The version is the INFO list's ifil sub-chunk, whose major number is 2 for
 * SoundFont 2.x and 3 for SF3 -- the marker SF3 writers set.  The engine's own
 * SF_IsCompressed reads further (the shdr sample-type bits); a file whose ifil
 * says 2 but whose samples are compressed is still skipped there, and is rare
 * enough not to parse pdta for here.  A RIFF sfbk with no readable ifil is
 * passed as 'sf2' and left to the engine.
 */
export function soundFontKind(bytes) {
  if (!(bytes instanceof Uint8Array) || bytes.byteLength < 12
      || fourcc(bytes, 0) !== 'RIFF' || fourcc(bytes, 8) !== 'sfbk') {
    return 'invalid';
  }
  let offset = 12;
  while (offset + 8 <= bytes.byteLength) {
    const id = fourcc(bytes, offset);
    const size = u32(bytes, offset + 4);
    const body = offset + 8;
    if (id === 'LIST' && body + 4 <= bytes.byteLength && fourcc(bytes, body) === 'INFO') {
      const end = Math.min(body + size, bytes.byteLength);
      let sub = body + 4;
      while (sub + 8 <= end) {
        const subId = fourcc(bytes, sub);
        const subSize = u32(bytes, sub + 4);
        if (subId === 'ifil' && subSize >= 4 && sub + 12 <= end) {
          const major = bytes[sub + 8] | (bytes[sub + 9] << 8);
          return major >= 3 ? 'sf3' : 'sf2';
        }
        sub += 8 + subSize + (subSize & 1);
      }
      return 'sf2';
    }
    offset = body + size + (size & 1);
  }
  return 'sf2';
}

/*
 * Settle one import batch's SoundFonts.  items are { path, bytes, source }
 * with path already mapped; every item mapped to SOUNDFONT_PATH is checked,
 * and of the usable ones only the last is kept -- all of them would otherwise
 * be written in turn to the same file, with the result depending on the order
 * the browser listed a folder in.  Returns the items to store and one note per
 * SoundFont decision, for the import message.
 */
export function settleSoundFonts(items) {
  const usable = [];
  const notes = [];
  const others = [];
  for (const item of items) {
    if (item.path !== SOUNDFONT_PATH) {
      others.push(item);
      continue;
    }
    const kind = soundFontKind(item.bytes);
    if (kind === 'sf3') {
      notes.push(`${item.source} is an SF3 (compressed) SoundFont, which the web build's MIDI synth cannot read; not imported.`);
    } else if (kind === 'invalid') {
      notes.push(`${item.source} is not a SoundFont; not imported.`);
    } else {
      usable.push(item);
    }
  }
  const kept = usable.at(-1);
  for (const item of usable.slice(0, -1)) {
    notes.push(`${item.source} was not used: one SoundFont is kept, and ${kept.source} came later.`);
  }
  if (kept) {
    notes.push(`SoundFont: ${kept.source}, for the MIDI music.`);
  }
  return { items: kept ? [...others, kept] : others, notes, soundFontKept: Boolean(kept) };
}
