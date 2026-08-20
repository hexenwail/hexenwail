export const PHONE_CONTROL_KEYCODES = Object.freeze({
  forward: 'w'.charCodeAt(0),
  back: 's'.charCodeAt(0),
  left: 'a'.charCodeAt(0),
  right: 'd'.charCodeAt(0),
  attack: 200, // K_MOUSE1
  jump: 32, // K_SPACE
  use: 251, // K_GP_LTHUMB, default binding "impulse 13" lift/use
  menu: 27, // K_ESCAPE
  nextWeapon: 248, // K_GP_RSHOULDER, default binding "impulse 10"
  prevWeapon: 247, // K_GP_LSHOULDER, default binding "impulse 12"
  run: 134, // K_SHIFT
});

export const DEFAULT_PHONE_CONTROL_OPTIONS = Object.freeze({
  deadZone: 0.24,
  releaseZone: 0.18,
  lookSensitivity: 1,
  maxLookDelta: 48,
  keys: PHONE_CONTROL_KEYCODES,
});

const BUTTON_ACTIONS = new Set(['attack', 'jump', 'use', 'menu', 'nextWeapon', 'prevWeapon', 'run']);

function clamp(value, min, max) {
  return Math.max(min, Math.min(max, value));
}

function eventPoint(event) {
  return { x: event.clientX ?? 0, y: event.clientY ?? 0 };
}

function actionForTarget(target) {
  return target?.closest?.('[data-phone-action]')?.dataset?.phoneAction ?? target?.dataset?.phoneAction ?? null;
}

function rectCenter(element, fallback) {
  const rect = element?.getBoundingClientRect?.();
  if (!rect || (!rect.width && !rect.height)) {
    return fallback;
  }
  return { x: rect.left + rect.width / 2, y: rect.top + rect.height / 2 };
}

export class PhoneControls {
  constructor(root, bridge, options = {}) {
    this.root = root;
    this.bridge = bridge;
    this.options = { ...DEFAULT_PHONE_CONTROL_OPTIONS, ...options };
    this.keys = { ...DEFAULT_PHONE_CONTROL_OPTIONS.keys, ...(options.keys ?? {}) };
    this.enabled = false;
    this.pointerOwners = new Map();
    this.heldKeys = new Set();
    this.stickKeys = new Set();
    this.stickCenter = null;
    this.lastLookPoint = null;

    this.bound = {
      pointerdown: (event) => this.onPointerDown(event),
      pointermove: (event) => this.onPointerMove(event),
      pointerup: (event) => this.onPointerEnd(event),
      pointercancel: (event) => this.onPointerEnd(event),
      lostpointercapture: (event) => this.onPointerEnd(event),
    };
  }

  attach() {
    if (!this.root) return;
    this.enabled = true;
    this.root.addEventListener('pointerdown', this.bound.pointerdown);
    this.root.addEventListener('pointermove', this.bound.pointermove);
    this.root.addEventListener('pointerup', this.bound.pointerup);
    this.root.addEventListener('pointercancel', this.bound.pointercancel);
    this.root.addEventListener('lostpointercapture', this.bound.lostpointercapture);
  }

  detach() {
    if (!this.root) return;
    this.releaseAll();
    this.enabled = false;
    this.root.removeEventListener('pointerdown', this.bound.pointerdown);
    this.root.removeEventListener('pointermove', this.bound.pointermove);
    this.root.removeEventListener('pointerup', this.bound.pointerup);
    this.root.removeEventListener('pointercancel', this.bound.pointercancel);
    this.root.removeEventListener('lostpointercapture', this.bound.lostpointercapture);
  }

  setLookSensitivity(value) {
    const parsed = Number(value);
    this.options.lookSensitivity = Number.isFinite(parsed) && parsed > 0 ? parsed : DEFAULT_PHONE_CONTROL_OPTIONS.lookSensitivity;
  }

  onPointerDown(event) {
    if (!this.enabled || !this.root) return;
    const action = actionForTarget(event.target);
    if (!action) return;
    if ([...this.pointerOwners.values()].some((owner) => owner.action === action)) return;

    event.preventDefault?.();
    event.stopPropagation?.();
    event.target?.setPointerCapture?.(event.pointerId);

    if (action === 'stick') {
      this.pointerOwners.set(event.pointerId, { type: 'stick', action });
      this.stickCenter = rectCenter(event.target, eventPoint(event));
      this.updateStick(event);
      return;
    }

    if (action === 'look') {
      this.pointerOwners.set(event.pointerId, { type: 'look', action });
      this.lastLookPoint = eventPoint(event);
      return;
    }

    if (BUTTON_ACTIONS.has(action)) {
      const key = this.keys[action];
      this.pointerOwners.set(event.pointerId, { type: 'button', action, key });
      if (key) this.pressKey(key);
    }
  }

  onPointerMove(event) {
    const owner = this.pointerOwners.get(event.pointerId);
    if (!owner) return;
    event.preventDefault?.();
    event.stopPropagation?.();

    if (owner.type === 'stick') {
      this.updateStick(event);
      return;
    }
    if (owner.type === 'look') {
      const point = eventPoint(event);
      if (this.lastLookPoint) {
        const scale = this.options.lookSensitivity;
        const dx = clamp(point.x - this.lastLookPoint.x, -this.options.maxLookDelta, this.options.maxLookDelta) * scale;
        const dy = clamp(point.y - this.lastLookPoint.y, -this.options.maxLookDelta, this.options.maxLookDelta) * scale;
        if (dx || dy) this.bridge.look(dx, dy);
      }
      this.lastLookPoint = point;
    }
  }

  onPointerEnd(event) {
    const owner = this.pointerOwners.get(event.pointerId);
    if (!owner) return;
    event.preventDefault?.();
    event.stopPropagation?.();
    event.target?.releasePointerCapture?.(event.pointerId);
    this.pointerOwners.delete(event.pointerId);

    if (owner.type === 'stick') {
      this.releaseStick();
      this.stickCenter = null;
    } else if (owner.type === 'look') {
      this.lastLookPoint = null;
    } else if (owner.type === 'button' && owner.key) {
      this.releaseKey(owner.key);
    }
  }

  updateStick(event) {
    const center = this.stickCenter ?? eventPoint(event);
    const point = eventPoint(event);
    const target = event.target?.closest?.('[data-phone-action="stick"]') ?? event.target;
    const rect = target?.getBoundingClientRect?.();
    const radius = Math.max(32, Math.min(rect?.width || 96, rect?.height || 96) / 2);
    const x = clamp((point.x - center.x) / radius, -1, 1);
    const y = clamp((point.y - center.y) / radius, -1, 1);
    const threshold = this.stickKeys.size ? this.options.releaseZone : this.options.deadZone;
    const nextKeys = new Set();
    if (y < -threshold) nextKeys.add(this.keys.forward);
    if (y > threshold) nextKeys.add(this.keys.back);
    if (x < -threshold) nextKeys.add(this.keys.left);
    if (x > threshold) nextKeys.add(this.keys.right);
    this.setStickKeys(nextKeys);
    this.root?.style?.setProperty('--stick-x', `${clamp(x, -1, 1) * 2}rem`);
    this.root?.style?.setProperty('--stick-y', `${clamp(y, -1, 1) * 2}rem`);
  }

  setStickKeys(nextKeys) {
    for (const key of this.stickKeys) {
      if (!nextKeys.has(key)) this.releaseKey(key);
    }
    for (const key of nextKeys) {
      if (!this.stickKeys.has(key)) this.pressKey(key);
    }
    this.stickKeys = nextKeys;
  }

  releaseStick() {
    this.setStickKeys(new Set());
    this.root?.style?.setProperty('--stick-x', '0px');
    this.root?.style?.setProperty('--stick-y', '0px');
  }

  pressKey(key) {
    if (this.heldKeys.has(key)) return;
    this.heldKeys.add(key);
    this.bridge.key(key, true);
  }

  releaseKey(key) {
    if (!this.heldKeys.has(key)) return;
    this.heldKeys.delete(key);
    this.bridge.key(key, false);
  }

  releaseAll() {
    this.pointerOwners.clear();
    this.releaseStick();
    for (const key of [...this.heldKeys]) {
      this.releaseKey(key);
    }
    this.lastLookPoint = null;
  }
}
