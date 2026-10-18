// Every requestPointerLock on the game canvas, not just the page's own.
//
// app.js's tryCaptureInput catches the promise Chrome returns (#150), but it
// is not the only caller.  The engine asks for the lock through SDL3: relative
// mouse mode (IN_ActivateMouse, the menu handing the pointer back, focus
// regained) is Emscripten_SetRelativeMouseMode -> emscripten_request_pointerlock,
// and a mouse button in relative mode with the lock lost regrabs the same way.
// Both end in library_html5.js's requestPointerLock helper, which calls
// target.requestPointerLock() and drops the promise.  Since emscripten 4 that
// helper runs straight from the main loop whenever navigator.userActivation is
// active, not only inside an input handler.  Chrome's "Pointer is already
// locked." is the browser process's kAlreadyLocked: Blink holds no lock of its
// own (so document.pointerLockElement is null and no page-side check sees it
// coming) while the browser still holds or is granting one -- for instance
// right after exitPointerLock(), which Blink applies synchronously and the
// browser later.  An engine that releases the pointer and takes it back
// lands there.  The rejection is InUseAttributeError, and with nobody holding
// the promise it reached the page's unhandledrejection handler as the status
// line.
//
// So the canvas's own requestPointerLock is wrapped: a call while the canvas
// already holds the lock is a no-op, and a refusal the next click repairs is
// logged, not raised.  Anything else is re-raised, so a real fault still
// reaches the global handler.  Safari returns nothing and is passed through.

const GUARDED = Symbol.for('hexenwail.pointerLockGuarded');

// Chrome's mapping of a refused lock to an exception lives in
// PointerLockController::ConvertResultToException (Blink).  Refusals here are
// ones the player repairs with the next click: the lock is already held or
// pending, the request had no user gesture or came too fast, or it landed
// inside the cooldown after Escape released it.  Missing from the list on
// purpose: WrongDocumentError, NotSupportedError, UnknownError, and the
// SecurityError Chrome raises for a document that can never be locked.
export function isBenignPointerLockRefusal(error) {
  const name = error?.name;
  if (name === 'InUseAttributeError' || name === 'NotAllowedError') {
    return true;
  }
  return name === 'SecurityError' && /exited the lock/i.test(error?.message ?? '');
}

export function guardPointerLock(element, doc = globalThis.document, log = console) {
  const original = element?.requestPointerLock;
  if (typeof original !== 'function' || original[GUARDED]) {
    return false;
  }
  function requestPointerLock(...args) {
    // Options would make this a request to change the lock, not take it.
    if (args.length === 0 && doc?.pointerLockElement === element) {
      return Promise.resolve();
    }
    const pending = original.apply(element, args);
    if (typeof pending?.then !== 'function') {
      return pending;
    }
    return pending.then(undefined, (error) => {
      if (isBenignPointerLockRefusal(error)) {
        log?.debug?.('Pointer lock not granted:', error?.message ?? error);
        return undefined;
      }
      throw error;
    });
  }
  requestPointerLock[GUARDED] = true;
  element.requestPointerLock = requestPointerLock;
  return true;
}
