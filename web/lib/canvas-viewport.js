// Is the game canvas actually on screen?  (GitHub #145)
//
// The WebGL2 self-test renders offscreen, so it passed all through the o6js
// layout bug (721c218b2), when the canvas was healthy but laid out off the
// bottom of the page.  This checks the canvas box against the visual
// viewport -- the part of the page the player can see, which on a phone with
// the keyboard up or pinch-zoom is smaller than the layout viewport -- so the
// launcher can say so in its runtime log the moment the game starts.

// getBoundingClientRect() is in layout-viewport coordinates.  The visual
// viewport sits inside the layout viewport at (offsetLeft, offsetTop).
export function readViewport(win = globalThis) {
  const vv = win.visualViewport;
  if (vv && vv.width > 0 && vv.height > 0) {
    return {
      left: vv.offsetLeft || 0, top: vv.offsetTop || 0, width: vv.width, height: vv.height, scale: vv.scale || 1,
    };
  }
  return { left: 0, top: 0, width: win.innerWidth || 0, height: win.innerHeight || 0, scale: 1 };
}

// Sub-pixel layout (fractional devicePixelRatio, rounded CSS sizes) leaves a
// canvas a fraction of a pixel over the edge.  That is not a layout bug.
const DEFAULT_TOLERANCE = 1;

const round = (v) => Math.round(v * 10) / 10;

// rect: {left, top, width, height} as from getBoundingClientRect().
// viewport: {left, top, width, height} as from readViewport().
// Returns {ok, overflow: {left, top, right, bottom}, visibleFraction, message}.
export function canvasViewportVerdict(rect, viewport, tolerance = DEFAULT_TOLERANCE) {
  const vRight = viewport.left + viewport.width;
  const vBottom = viewport.top + viewport.height;
  const right = rect.left + rect.width;
  const bottom = rect.top + rect.height;

  const overflow = {
    left: Math.max(0, viewport.left - rect.left),
    top: Math.max(0, viewport.top - rect.top),
    right: Math.max(0, right - vRight),
    bottom: Math.max(0, bottom - vBottom),
  };

  const area = rect.width * rect.height;
  const visW = Math.max(0, Math.min(right, vRight) - Math.max(rect.left, viewport.left));
  const visH = Math.max(0, Math.min(bottom, vBottom) - Math.max(rect.top, viewport.top));
  const visibleFraction = area > 0 ? (visW * visH) / area : 0;

  const box = `canvas ${round(rect.width)}x${round(rect.height)} at (${round(rect.left)}, ${round(rect.top)})`;
  const view = `visual viewport ${round(viewport.width)}x${round(viewport.height)} at (${round(viewport.left)}, ${round(viewport.top)})`;

  if (!(rect.width > 0 && rect.height > 0)) {
    return { ok: false, overflow, visibleFraction: 0, message: `${box} has no area; ${view}` };
  }

  const over = Object.entries(overflow).filter(([, px]) => px > tolerance);
  if (over.length === 0) {
    return { ok: true, overflow, visibleFraction, message: `${box} is inside the ${view}` };
  }
  const edges = over.map(([edge, px]) => `${edge} by ${round(px)}px`).join(', ');
  // A page pinch-zoomed before launch (iOS ignores user-scalable=no) really
  // does put the canvas partly off-screen, but that is the player's zoom, not
  // a layout bug, and the log should say which it is.
  const zoom = viewport.scale && Math.abs(viewport.scale - 1) > 0.01
    ? `; page is zoomed ${round(viewport.scale)}x, which may be the cause` : '';
  return {
    ok: false,
    overflow,
    visibleFraction,
    message: `${box} sticks out of the ${view}: ${edges}; ${Math.round(visibleFraction * 100)}% visible${zoom}`,
  };
}
