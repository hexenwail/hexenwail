import { extractZipEntries } from './lib/zip.js';
import { hasRequiredBaseAssets, mapImportedPath } from './lib/paths.js';
import {
  createSaveBundle, getPakCompatibilityWarnings, isSavePath, planSaveImport, sha256, validateSaveBundle,
} from './lib/save-bundle.js';

const BASE_DIR = '/persistent';
const STORAGE_ROOT = 'hexenwail';
const SAVE_SYNC_INTERVAL_MS = 10000;
const MAX_IMPORT_BYTES = 2 * 1024 * 1024 * 1024;

const state = {
  storage: null,
  runtimeReady: false,
  runtimeLoaded: false,
  storageReady: false,
  engineStarted: false,
  syncing: false,
  syncPromise: null,
  applyingSaveImport: false,
  storedPaths: new Set(),
  runtimeSnapshot: new Map(),
  lastStatus: 'Preparing launcher…',
};

const ui = {};

function getBoot() {
  if (!globalThis.HexenwailBoot) {
    globalThis.HexenwailBoot = {};
  }
  return globalThis.HexenwailBoot;
}

function getModule() {
  if (!globalThis.Module) {
    globalThis.Module = {
      preRun: [],
      postRun: [],
      arguments: ['-basedir', BASE_DIR],
      noInitialRun: true,
      locateFile: (path) => new URL(path, document.baseURI).toString(),
    };
  }
  return globalThis.Module;
}

function getFS() {
  return globalThis.FS || getModule().FS;
}

function setStatus(message, kind = 'info') {
  state.lastStatus = message;
  if (ui.statusText) {
    ui.statusText.textContent = message;
  }
  if (ui.statusPanel) {
    ui.statusPanel.dataset.kind = kind;
  }
}

function logToConsole(prefix, message, error = false) {
  const text = typeof message === 'string' ? message : String(message ?? '');
  if (error) {
    console.error(prefix, text);
  } else {
    console.log(prefix, text);
  }
}

function updateLaunchState() {
  const ready = hasRequiredBaseAssets([...state.storedPaths]);
  if (ui.launchButton) {
    ui.launchButton.disabled = !ready;
    ui.launchButton.textContent = state.engineStarted ? 'Running' : ready ? 'Start game' : 'Import pak0.pak + pak1.pak first';
  }
  if (ui.requirementsText) {
    ui.requirementsText.textContent = ready
      ? 'Required base game assets detected.'
      : 'Required: data1/pak0.pak and data1/pak1.pak from a legal Hexen II installation.';
  }
}

function formatBytes(bytes) {
  if (!Number.isFinite(bytes) || bytes < 0) {
    return 'unknown';
  }
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let value = bytes;
  let index = 0;
  while (value >= 1024 && index < units.length - 1) {
    value /= 1024;
    index += 1;
  }
  return `${value.toFixed(value >= 10 || index === 0 ? 0 : 1)} ${units[index]}`;
}

function setImportMessage(message, kind = 'info') {
  if (ui.importMessage) {
    ui.importMessage.textContent = message;
    ui.importMessage.dataset.kind = kind;
  }
}

function setSaveMessage(message, kind = 'info') {
  if (ui.saveMessage) {
    ui.saveMessage.textContent = message;
    ui.saveMessage.dataset.kind = kind;
  }
}

async function updateStorageIndicator() {
  if (!navigator.storage || !ui.storageText) {
    return;
  }

  try {
    const [estimate, persisted] = await Promise.all([
      navigator.storage.estimate?.(),
      navigator.storage.persisted?.(),
    ]);
    const usage = estimate?.usage ?? 0;
    const quota = estimate?.quota ?? 0;
    ui.storageText.textContent = `Browser storage: ${formatBytes(usage)} used / ${formatBytes(quota)} quota · persistence ${persisted ? 'granted' : 'best-effort'}`;
  } catch (error) {
    ui.storageText.textContent = 'Browser storage estimate unavailable.';
    console.warn(error);
  }
}

async function requestPersistentStorage() {
  if (!navigator.storage?.persist) {
    return false;
  }
  try {
    return await navigator.storage.persist();
  } catch (error) {
    console.warn('Persistent storage request failed', error);
    return false;
  }
}

async function ensureRuntimeDirectory(path) {
  const FS = getFS();
  const parts = path.split('/').filter(Boolean);
  let current = '';
  for (const part of parts) {
    current += `/${part}`;
    try {
      FS.mkdir(current);
    } catch (error) {
      if (!String(error).includes('File exists')) {
        try {
          FS.lookupPath(current);
        } catch {
          throw error;
        }
      }
    }
  }
}

function relativeFromBase(absolutePath) {
  if (absolutePath === BASE_DIR) {
    return '';
  }
  return absolutePath.startsWith(`${BASE_DIR}/`) ? absolutePath.slice(BASE_DIR.length + 1) : absolutePath;
}

function toMtimeMs(value) {
  if (typeof value === 'number') {
    return value;
  }
  if (value?.getTime) {
    return value.getTime();
  }
  return Date.now();
}

async function writeRuntimeFile(relativePath, bytes) {
  const FS = getFS();
  const absolutePath = `${BASE_DIR}/${relativePath}`;
  const directory = absolutePath.split('/').slice(0, -1).join('/');
  await ensureRuntimeDirectory(directory);
  FS.writeFile(absolutePath, bytes);
  const stat = FS.stat(absolutePath);
  state.runtimeSnapshot.set(relativePath, { size: stat.size, mtimeMs: toMtimeMs(stat.mtime) });
}

function walkRuntimeFiles(path = BASE_DIR, files = []) {
  const FS = getFS();
  for (const name of FS.readdir(path)) {
    if (name === '.' || name === '..') {
      continue;
    }
    const child = `${path}/${name}`;
    const stat = FS.stat(child);
    if (FS.isDir(stat.mode)) {
      walkRuntimeFiles(child, files);
    } else {
      files.push({ absolutePath: child, stat });
    }
  }
  return files;
}

function normalizeStoredMetadata(stat) {
  return {
    size: stat.size,
    mtimeMs: toMtimeMs(stat.mtime),
  };
}

function removeRuntimeTree(path) {
  const FS = getFS();
  for (const name of FS.readdir(path)) {
    if (name === '.' || name === '..') {
      continue;
    }
    const child = `${path}/${name}`;
    const stat = FS.stat(child);
    if (FS.isDir(stat.mode)) {
      removeRuntimeTree(child);
      FS.rmdir(child);
    } else {
      FS.unlink(child);
    }
  }
}

async function syncRuntimeToStorage() {
  if (!state.runtimeReady || !state.storageReady || state.syncing || state.applyingSaveImport) {
    return state.syncPromise;
  }

  state.syncPromise = (async () => {
    state.syncing = true;
    try {
    const FS = getFS();
    const present = new Set();
    const runtimeFiles = walkRuntimeFiles();

    for (const { absolutePath, stat } of runtimeFiles) {
      const relativePath = relativeFromBase(absolutePath);
      present.add(relativePath);
      const meta = normalizeStoredMetadata(stat);
      const previous = state.runtimeSnapshot.get(relativePath);
      if (!previous || previous.size !== meta.size || previous.mtimeMs !== meta.mtimeMs) {
        const bytes = FS.readFile(absolutePath);
        await state.storage.writeFile(relativePath, bytes, meta);
        state.runtimeSnapshot.set(relativePath, meta);
        state.storedPaths.add(relativePath);
      }
    }

    for (const relativePath of [...state.runtimeSnapshot.keys()]) {
      if (!present.has(relativePath)) {
        await state.storage.deleteFile(relativePath);
        state.runtimeSnapshot.delete(relativePath);
        state.storedPaths.delete(relativePath);
      }
    }
    } finally {
      state.syncing = false;
      updateLaunchState();
      updateStorageIndicator();
    }
  })();
  return state.syncPromise;
}

async function loadStoredFilesIntoRuntime() {
  if (!state.runtimeReady || !state.storageReady) {
    return;
  }

  const files = await state.storage.listFiles();
  state.storedPaths = new Set(files.map((file) => file.path));
  setStatus(files.length ? 'Restoring imported game data…' : 'Waiting for Hexen II assets…');
  await ensureRuntimeDirectory(BASE_DIR);

  let restoredBytes = 0;
  let index = 0;
  for (const file of files) {
    const bytes = await state.storage.readFile(file.path);
    await writeRuntimeFile(file.path, bytes);
    restoredBytes += bytes.byteLength;
    index += 1;
    if (ui.progressText) {
      ui.progressText.textContent = `Restored ${index}/${files.length} files (${formatBytes(restoredBytes)})`;
    }
  }

  updateLaunchState();
  if (!files.length && ui.progressText) {
    ui.progressText.textContent = 'No imported assets yet.';
  }
}

function isZipFile(file) {
  return file.name.toLowerCase().endsWith('.zip');
}

function collectLooseFiles(fileList) {
  return Array.from(fileList, (file) => ({
    sourcePath: file.webkitRelativePath || file.name,
    file,
  }));
}

async function importFileBatch(entries) {
  let processedBytes = 0;
  const accepted = [];
  const rejected = [];

  for (const entry of entries) {
    const mappedPath = mapImportedPath(entry.sourcePath);
    if (!mappedPath) {
      rejected.push(entry.sourcePath);
      continue;
    }
    const bytes = new Uint8Array(await entry.file.arrayBuffer());
    processedBytes += bytes.byteLength;
    accepted.push({ path: mappedPath, bytes });
    if (ui.progressText) {
      ui.progressText.textContent = `Imported ${accepted.length} files (${formatBytes(processedBytes)})`;
    }
  }

  for (const item of accepted) {
    await state.storage.writeFile(item.path, item.bytes, { size: item.bytes.byteLength, mtimeMs: Date.now() });
    state.storedPaths.add(item.path);
    if (state.runtimeReady) {
      await writeRuntimeFile(item.path, item.bytes);
    }
  }

  return { accepted, rejected };
}

async function importZipFile(file) {
  if (file.size > MAX_IMPORT_BYTES) {
    throw new Error(`ZIP file is too large for browser storage (${formatBytes(file.size)}).`);
  }

  const archiveBytes = new Uint8Array(await file.arrayBuffer());
  const extracted = await extractZipEntries(archiveBytes, {
    onProgress: ({ processedBytes, totalBytes, entry }) => {
      if (ui.progressText) {
        ui.progressText.textContent = `Extracting ${entry.rawName} (${formatBytes(processedBytes)} / ${formatBytes(totalBytes)})`;
      }
    },
  });

  const accepted = [];
  const rejected = [];
  for (const entry of extracted) {
    const mappedPath = mapImportedPath(entry.name);
    if (!mappedPath) {
      rejected.push(entry.rawName);
      continue;
    }
    accepted.push({ path: mappedPath, bytes: entry.data });
  }

  let processedBytes = 0;
  for (const item of accepted) {
    processedBytes += item.bytes.byteLength;
    await state.storage.writeFile(item.path, item.bytes, { size: item.bytes.byteLength, mtimeMs: Date.now() });
    state.storedPaths.add(item.path);
    if (state.runtimeReady) {
      await writeRuntimeFile(item.path, item.bytes);
    }
    if (ui.progressText) {
      ui.progressText.textContent = `Stored ${accepted.length} extracted files (${formatBytes(processedBytes)})`;
    }
  }

  return { accepted, rejected };
}

async function handleImportedFiles(fileList) {
  if (!fileList?.length) {
    return;
  }

  setImportMessage('Importing assets…');
  const zipFiles = [];
  const looseFiles = [];
  for (const entry of collectLooseFiles(fileList)) {
    if (isZipFile(entry.file)) {
      zipFiles.push(entry.file);
    } else {
      looseFiles.push(entry);
    }
  }

  const accepted = [];
  const rejected = [];
  for (const file of zipFiles) {
    const result = await importZipFile(file);
    accepted.push(...result.accepted.map((item) => item.path));
    rejected.push(...result.rejected);
  }
  if (looseFiles.length) {
    const result = await importFileBatch(looseFiles);
    accepted.push(...result.accepted.map((item) => item.path));
    rejected.push(...result.rejected);
  }

  await updateStorageIndicator();
  updateLaunchState();
  await maybeStartEngine();

  if (accepted.length) {
    setImportMessage(`Imported ${accepted.length} file(s). Existing files were replaced when names matched.`, 'success');
  } else {
    setImportMessage('No recognized Hexen II assets were imported.', 'error');
  }
  if (rejected.length && ui.rejectedList) {
    ui.rejectedList.textContent = `Ignored ${rejected.length} item(s): ${rejected.slice(0, 8).join(', ')}${rejected.length > 8 ? '…' : ''}`;
  } else if (ui.rejectedList) {
    ui.rejectedList.textContent = '';
  }
}

async function getInstalledPaks(gameDirectories) {
  const paks = [];
  const normalizedDirs = gameDirectories ? gameDirectories.map(d => d.toLowerCase()) : null;
  for (const entry of await state.storage.listFiles()) {
    if (!/^(?:data1|portals|hw)\/.*\.pak$/i.test(entry.path)) continue;
    if (normalizedDirs && !normalizedDirs.includes(entry.path.split('/')[0].toLowerCase())) continue;
    const bytes = await state.storage.readFile(entry.path);
    paks.push({ path: entry.path, size: bytes.byteLength, sha256: await sha256(bytes) });
  }
  return paks;
}

async function exportSaves() {
  try {
    setSaveMessage('Syncing recent saves to local storage…');
    await syncRuntimeToStorage();
    const stored = await state.storage.listFiles();
    const saveEntries = [];
    for (const entry of stored) {
      if (!isSavePath(entry.path)) continue;
      saveEntries.push({ path: entry.path, bytes: await state.storage.readFile(entry.path) });
    }
    if (!saveEntries.length) {
      setSaveMessage('No save files were found. Save a game first, then return to the launcher and export.', 'error');
      return;
    }
    setSaveMessage(`Preparing ${saveEntries.length} save file(s)…`);
    const { bytes } = await createSaveBundle(saveEntries, {
      build: globalThis.HEXENWAIL_BUILD,
      requiredPaks: await getInstalledPaks([...new Set(saveEntries.map((entry) => entry.path.split('/')[0]))]),
    });
    const date = new Date().toISOString().slice(0, 10);
    const filename = `hexenwail-saves-${date}.hexenwail-save.zip`;
    const file = new File([bytes], filename, { type: 'application/zip' });
    if (navigator.share && navigator.canShare?.({ files: [file] })) {
      try {
        await navigator.share({ files: [file], title: 'Hexenwail save bundle' });
        setSaveMessage(`Exported ${saveEntries.length} save file(s), ${formatBytes(bytes.byteLength)}.`, 'success');
        return;
      } catch (error) {
        if (error.name === 'AbortError') {
          setSaveMessage('Save export was cancelled.');
          return;
        }
      }
    }
    const link = document.createElement('a');
    link.href = URL.createObjectURL(file);
    link.download = filename;
    link.click();
    setTimeout(() => URL.revokeObjectURL(link.href), 1000);
    setSaveMessage(`Downloaded ${saveEntries.length} save file(s), ${formatBytes(bytes.byteLength)}. Use Files or iCloud Drive to move it.`, 'success');
  } catch (error) {
    setSaveMessage(`Save export failed: ${error.message}`, 'error');
  }
}

async function deleteRuntimeFile(relativePath) {
  if (!state.runtimeReady) return;
  try {
    getFS().unlink(`${BASE_DIR}/${relativePath}`);
  } catch (error) {
    if (!String(error).includes('No such file')) throw error;
  }
  state.runtimeSnapshot.delete(relativePath);
}

async function applySaveImport(bundle, mode) {
  await syncRuntimeToStorage();
  state.applyingSaveImport = true;
  try {
    const existing = (await state.storage.listFiles()).map((entry) => entry.path);
    const { writes, deletes } = planSaveImport(existing, bundle.files, mode);
    const affected = [...new Set([...writes, ...deletes])];
    const before = new Map();
    for (const path of affected) {
      if (existing.includes(path)) before.set(path, await state.storage.readFile(path));
    }
    try {
      for (const file of bundle.files) {
        if (!writes.includes(file.path)) continue;
        await state.storage.writeFile(file.path, file.bytes, { size: file.bytes.byteLength, mtimeMs: Date.now() });
        state.storedPaths.add(file.path);
      }
      for (const path of deletes) {
        await state.storage.deleteFile(path);
        state.storedPaths.delete(path);
      }
    } catch (error) {
      for (const path of affected) {
        if (before.has(path)) {
          const bytes = before.get(path);
          await state.storage.writeFile(path, bytes, { size: bytes.byteLength, mtimeMs: Date.now() }).catch(() => {});
          state.storedPaths.add(path);
        } else {
          await state.storage.deleteFile(path).catch(() => {});
          state.storedPaths.delete(path);
        }
      }
      throw new Error(`Save import could not be committed; prior saves were restored (${error.message})`);
    }
    try {
      for (const file of bundle.files) {
        if (!writes.includes(file.path)) continue;
        await writeRuntimeFile(file.path, file.bytes);
      }
      for (const path of deletes) await deleteRuntimeFile(path);
    } catch (error) {
      console.warn('Imported saves will be loaded after reload', error);
    }
    return { writes, deletes };
  } finally {
    state.applyingSaveImport = false;
  }
}

async function importSaveBundle(file) {
  try {
    if (file.size > 256 * 1024 * 1024) throw new Error('Save bundle is too large.');
    setSaveMessage('Checking save bundle…');
    const bundle = await validateSaveBundle(new Uint8Array(await file.arrayBuffer()));
    const warnings = getPakCompatibilityWarnings(bundle.manifest.requiredPaks, await getInstalledPaks(bundle.manifest.gameDirectories));
    const size = bundle.files.reduce((total, entry) => total + entry.bytes.byteLength, 0);
    const mode = ui.saveImportMode?.value === 'replace' ? 'replace' : 'merge';
    const summary = `Import ${bundle.files.length} save file(s) (${formatBytes(size)}) from ${new Date(bundle.manifest.createdAt).toLocaleString()} for ${bundle.manifest.gameDirectories.join(', ')}?\n\nMode: ${mode === 'replace' ? 'Replace saves (all existing save slots are removed)' : 'Merge (only matching save paths are replaced)'}${warnings.length ? `\n\nCompatibility warnings:\n${warnings.join('\n')}` : ''}`;
    setSaveMessage(`Ready to import ${bundle.files.length} save file(s) from ${new Date(bundle.manifest.createdAt).toLocaleString()}.${warnings.length ? ` ${warnings.join(' ')}` : ''}`);
    if (!confirm(summary)) return;
    const result = await applySaveImport(bundle, mode);
    setSaveMessage(`Imported ${result.writes.length} save file(s)${result.deletes.length ? ` and removed ${result.deletes.length} existing save file(s)` : ''}.${state.engineStarted ? ' Restart/reload before loading saves to avoid an active-game race.' : ''}`, 'success');
  } catch (error) {
    setSaveMessage(`Save import failed: ${error.message}`, 'error');
  }
}

async function maybeStartEngine() {
  if (state.engineStarted || !state.runtimeReady || !state.storageReady || !hasRequiredBaseAssets([...state.storedPaths])) {
    return;
  }
  state.engineStarted = true;
  document.body.dataset.engineState = 'running';
  setStatus('Starting Hexenwail…');
  try {
    getModule().callMain?.([]);
    setStatus('Hexenwail running. Tap the canvas to focus input.');
  } catch (error) {
    state.engineStarted = false;
    document.body.dataset.engineState = 'ready';
    setStatus(`Engine start failed: ${error.message}`, 'error');
    throw error;
  }
}

async function clearImportedData() {
  if (!confirm('This removes all imported Hexen II assets AND any local save games from this browser. This cannot be undone. Continue?')) {
    return;
  }
  await state.storage.clear();
  state.storedPaths.clear();
  state.runtimeSnapshot.clear();
  if (state.runtimeReady) {
    try {
      const FS = getFS();
      for (const name of FS.readdir(BASE_DIR)) {
        if (name === '.' || name === '..') continue;
        const child = `${BASE_DIR}/${name}`;
        const stat = FS.stat(child);
        if (FS.isDir(stat.mode)) {
          removeRuntimeTree(child);
          FS.rmdir(child);
        } else {
          FS.unlink(child);
        }
      }
    } catch (error) {
      console.warn('Runtime clear failed', error);
    }
  }
  state.engineStarted = false;
  document.body.dataset.engineState = 'ready';
  setImportMessage('Imported browser data cleared.', 'success');
  if (ui.rejectedList) ui.rejectedList.textContent = '';
  if (ui.progressText) ui.progressText.textContent = 'No imported assets yet.';
  updateLaunchState();
  updateStorageIndicator();
}

async function requestFullscreenForCanvas() {
  const canvas = ui.canvas;
  if (!canvas) return;

  let target = null;
  let method = null;
  if (typeof canvas.requestFullscreen === 'function') {
    target = canvas;
    method = canvas.requestFullscreen;
  } else if (typeof canvas.webkitRequestFullscreen === 'function') {
    target = canvas;
    method = canvas.webkitRequestFullscreen;
  } else if (typeof document.documentElement.requestFullscreen === 'function') {
    target = document.documentElement;
    method = document.documentElement.requestFullscreen;
  }

  if (!method) {
    setImportMessage('Fullscreen is not supported by this browser. Installed iPadOS PWAs already run nearly edge-to-edge.', 'info');
    return;
  }
  try {
    await method.call(target);
  } catch (error) {
    setImportMessage(`Fullscreen request failed: ${error.message}`, 'error');
  }
}

function tryCaptureInput() {
  if (ui.canvas) {
    ui.canvas.focus();
    if (typeof ui.canvas.requestPointerLock === 'function') {
      ui.canvas.requestPointerLock();
    } else if (ui.pointerLockHint) {
      ui.pointerLockHint.textContent = 'Pointer Lock is unavailable here (expected on iPadOS Safari). External keyboard/mouse still work without it.';
    }
  }
}

function createOPFSBackend(rootHandle) {
  async function getDirectoryHandle(relativePath, create = false) {
    let handle = rootHandle;
    for (const segment of relativePath.split('/').filter(Boolean)) {
      handle = await handle.getDirectoryHandle(segment, { create });
    }
    return handle;
  }

  async function getFileHandle(relativePath, create = false) {
    const segments = relativePath.split('/').filter(Boolean);
    const fileName = segments.pop();
    const dir = await getDirectoryHandle(segments.join('/'), create);
    return dir.getFileHandle(fileName, { create });
  }

  async function listRecursive(handle, prefix = '') {
    const items = [];
    for await (const entry of handle.values()) {
      const path = prefix ? `${prefix}/${entry.name}` : entry.name;
      if (entry.kind === 'directory') {
        items.push(...await listRecursive(entry, path));
      } else {
        const file = await entry.getFile();
        items.push({ path, size: file.size, mtimeMs: file.lastModified });
      }
    }
    return items;
  }

  return {
    async listFiles() {
      return listRecursive(rootHandle);
    },
    async readFile(relativePath) {
      const file = await (await getFileHandle(relativePath, false)).getFile();
      return new Uint8Array(await file.arrayBuffer());
    },
    async writeFile(relativePath, bytes) {
      const handle = await getFileHandle(relativePath, true);
      const writable = await handle.createWritable();
      await writable.write(bytes);
      await writable.close();
    },
    async deleteFile(relativePath) {
      const segments = relativePath.split('/').filter(Boolean);
      const fileName = segments.pop();
      const dir = await getDirectoryHandle(segments.join('/'), false).catch(() => null);
      if (dir) {
        await dir.removeEntry(fileName).catch(() => {});
      }
    },
    async clear() {
      const rootEntries = [];
      for await (const entry of rootHandle.values()) {
        rootEntries.push(entry.name);
      }
      for (const name of rootEntries) {
        await rootHandle.removeEntry(name, { recursive: true });
      }
    },
  };
}

function createIDBBackend() {
  const DB_NAME = 'hexenwail-pwa';
  const STORE_NAME = 'files';

  const openDatabase = () => new Promise((resolve, reject) => {
    const request = indexedDB.open(DB_NAME, 1);
    request.onupgradeneeded = () => {
      const db = request.result;
      if (!db.objectStoreNames.contains(STORE_NAME)) {
        db.createObjectStore(STORE_NAME, { keyPath: 'path' });
      }
    };
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
  });

  const run = async (mode, executor) => {
    const db = await openDatabase();
    return new Promise((resolve, reject) => {
      const tx = db.transaction(STORE_NAME, mode);
      const store = tx.objectStore(STORE_NAME);
      executor(store, resolve, reject);
      // The executor's own request handler (onsuccess/onerror) always settles
      // this promise before the transaction completes; oncomplete here is
      // solely responsible for closing this per-call connection.
      tx.oncomplete = () => db.close();
      tx.onerror = () => reject(tx.error);
    });
  };

  return {
    async listFiles() {
      const db = await openDatabase();
      return new Promise((resolve, reject) => {
        const tx = db.transaction(STORE_NAME, 'readonly');
        const request = tx.objectStore(STORE_NAME).getAll();
        request.onsuccess = () => {
          db.close();
          resolve(request.result.map(({ path, size, mtimeMs }) => ({ path, size, mtimeMs })));
        };
        request.onerror = () => reject(request.error);
      });
    },
    async readFile(relativePath) {
      return run('readonly', (store, resolve, reject) => {
        const request = store.get(relativePath);
        request.onsuccess = () => {
          if (!request.result) {
            reject(new Error(`Missing IndexedDB file: ${relativePath}`));
            return;
          }
          resolve(new Uint8Array(request.result.bytes));
        };
        request.onerror = () => reject(request.error);
      });
    },
    async writeFile(relativePath, bytes, meta = {}) {
      return run('readwrite', (store, resolve, reject) => {
        const request = store.put({ path: relativePath, bytes, size: meta.size ?? bytes.byteLength, mtimeMs: meta.mtimeMs ?? Date.now() });
        request.onsuccess = () => resolve();
        request.onerror = () => reject(request.error);
      });
    },
    async deleteFile(relativePath) {
      return run('readwrite', (store, resolve, reject) => {
        const request = store.delete(relativePath);
        request.onsuccess = () => resolve();
        request.onerror = () => reject(request.error);
      });
    },
    async clear() {
      return run('readwrite', (store, resolve, reject) => {
        const request = store.clear();
        request.onsuccess = () => resolve();
        request.onerror = () => reject(request.error);
      });
    },
  };
}

async function initStorageBackend() {
  if (navigator.storage?.getDirectory) {
    const root = await navigator.storage.getDirectory();
    const appRoot = await root.getDirectoryHandle(STORAGE_ROOT, { create: true });
    state.storage = createOPFSBackend(appRoot);
    if (ui.storageBackendText) ui.storageBackendText.textContent = 'Storage backend: OPFS';
    return;
  }

  state.storage = createIDBBackend();
  if (ui.storageBackendText) ui.storageBackendText.textContent = 'Storage backend: IndexedDB fallback';
}

async function registerServiceWorker() {
  if (!('serviceWorker' in navigator) || !ui.offlineText) {
    return;
  }

  try {
    const registration = await navigator.serviceWorker.register('./sw.js', { scope: './' });
    if (navigator.serviceWorker.controller) {
      ui.offlineText.textContent = 'Offline shell ready. Imported game data stays outside the service worker cache.';
    } else if (registration.active || registration.installing || registration.waiting) {
      ui.offlineText.textContent = 'Offline shell installing… reload once to let it take control of this tab.';
    } else {
      ui.offlineText.textContent = 'Installing offline shell… keep this tab open until the status changes.';
    }
    navigator.serviceWorker.addEventListener('controllerchange', () => {
      ui.offlineText.textContent = 'Offline shell ready. Imported game data stays outside the service worker cache.';
    });
  } catch (error) {
    ui.offlineText.textContent = `Offline shell unavailable: ${error.message}`;
  }
}

async function ensureEngineScriptLoaded() {
  if (state.runtimeLoaded || globalThis.__HEXENWAIL_EMSCRIPTEN_SCRIPT_EMBEDDED) {
    state.runtimeLoaded = true;
    return;
  }

  await new Promise((resolve, reject) => {
    const script = document.createElement('script');
    script.src = './hexenwail.js';
    script.defer = true;
    script.onload = () => {
      state.runtimeLoaded = true;
      resolve();
    };
    script.onerror = () => reject(new Error('Failed to load hexenwail.js. Build the WASM target before serving this directory.'));
    document.head.append(script);
  });
}

function bindUi() {
  Object.assign(ui, {
    canvas: document.getElementById('canvas'),
    statusPanel: document.getElementById('status-panel'),
    statusText: document.getElementById('status-text'),
    progressText: document.getElementById('progress-text'),
    importMessage: document.getElementById('import-message'),
    rejectedList: document.getElementById('rejected-list'),
    launchButton: document.getElementById('launch-button'),
    importButton: document.getElementById('import-button'),
    directoryButton: document.getElementById('directory-button'),
    fullscreenButton: document.getElementById('fullscreen-button'),
    clearButton: document.getElementById('clear-button'),
    fileInput: document.getElementById('file-input'),
    saveFileInput: document.getElementById('save-file-input'),
    exportSavesButton: document.getElementById('export-saves-button'),
    importSavesButton: document.getElementById('import-saves-button'),
    saveImportMode: document.getElementById('save-import-mode'),
    saveMessage: document.getElementById('save-message'),
    directoryInput: document.getElementById('directory-input'),
    dropZone: document.getElementById('drop-zone'),
    storageText: document.getElementById('storage-text'),
    storageBackendText: document.getElementById('storage-backend-text'),
    offlineText: document.getElementById('offline-text'),
    requirementsText: document.getElementById('requirements-text'),
    pointerLockHint: document.getElementById('pointer-lock-hint'),
  });

  ui.importButton?.addEventListener('click', () => ui.fileInput?.click());
  ui.directoryButton?.addEventListener('click', () => ui.directoryInput?.click());
  ui.fileInput?.addEventListener('change', async (event) => {
    await handleImportedFiles(event.target.files);
    event.target.value = '';
  });
  ui.directoryInput?.addEventListener('change', async (event) => {
    await handleImportedFiles(event.target.files);
    event.target.value = '';
  });
  ui.launchButton?.addEventListener('click', () => maybeStartEngine());
  ui.clearButton?.addEventListener('click', () => clearImportedData());
  ui.exportSavesButton?.addEventListener('click', () => exportSaves());
  ui.importSavesButton?.addEventListener('click', () => ui.saveFileInput?.click());
  ui.saveFileInput?.addEventListener('change', async (event) => {
    const [file] = event.target.files;
    if (file) await importSaveBundle(file);
    event.target.value = '';
  });
  ui.fullscreenButton?.addEventListener('click', () => requestFullscreenForCanvas());
  ui.canvas?.addEventListener('click', tryCaptureInput);

  if (ui.dropZone) {
    for (const eventName of ['dragenter', 'dragover']) {
      ui.dropZone.addEventListener(eventName, (event) => {
        event.preventDefault();
        ui.dropZone.dataset.drag = 'true';
      });
    }
    for (const eventName of ['dragleave', 'drop']) {
      ui.dropZone.addEventListener(eventName, (event) => {
        event.preventDefault();
        ui.dropZone.dataset.drag = 'false';
      });
    }
    ui.dropZone.addEventListener('drop', async (event) => {
      await handleImportedFiles(event.dataTransfer?.files ?? []);
    });
  }
}

function bindBootCallbacks() {
  const boot = getBoot();
  const Module = getModule();
  const previousOnRuntimeInitialized = Module.onRuntimeInitialized;
  Module.canvas = ui.canvas;
  Module.arguments = ['-basedir', BASE_DIR];
  Module.noInitialRun = true;
  Module.locateFile = (path) => new URL(path, document.baseURI).toString();
  Module.print = (text) => {
    logToConsole('[hexenwail]', text, false);
    boot.lastPrint = text;
  };
  Module.printErr = (text) => {
    if (!hasRequiredBaseAssets([...state.storedPaths]) && String(text).includes('Unable to find a proper Hexen II installation')) {
      return;
    }
    logToConsole('[hexenwail:error]', text, true);
  };
  Module.setStatus = (text) => {
    if (text) {
      setStatus(text);
    }
  };
  Module.monitorRunDependencies = (left) => {
    if (left > 0) {
      setStatus(`Loading engine runtime… ${left} remaining`);
    }
  };
  Module.onRuntimeInitialized = async () => {
    previousOnRuntimeInitialized?.();
    state.runtimeReady = true;
    boot.runtimeInitialized = true;
    setStatus('Engine runtime ready. Restoring persistent data…');
    await loadStoredFilesIntoRuntime();
    await maybeStartEngine();
  };

  if (boot.runtimeInitialized) {
    queueMicrotask(async () => {
      state.runtimeReady = true;
      await loadStoredFilesIntoRuntime();
      await maybeStartEngine();
    });
  }
}

async function init() {
  bindUi();
  bindBootCallbacks();
  updateLaunchState();
  setImportMessage('Import files, a directory, or a ZIP archive. All processing stays in your browser.');
  await requestPersistentStorage();
  await initStorageBackend();
  state.storageReady = true;
  await updateStorageIndicator();
  await registerServiceWorker();
  await ensureEngineScriptLoaded();
  if (state.runtimeReady) {
    await loadStoredFilesIntoRuntime();
    await maybeStartEngine();
  }

  setInterval(() => {
    syncRuntimeToStorage().catch((error) => console.warn('Save sync failed', error));
  }, SAVE_SYNC_INTERVAL_MS);
  addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'hidden') {
      syncRuntimeToStorage().catch((error) => console.warn('Save sync failed', error));
    }
  });
  // 'pagehide' fires on tab close, app switch (iPadOS Safari/PWA backgrounding),
  // and bfcache eviction; 'beforeunload' and 'freeze' are extra safety nets so
  // savegames written just before the runtime is suspended are not lost.
  addEventListener('pagehide', () => {
    syncRuntimeToStorage().catch((error) => console.warn('Save sync failed', error));
  });
  addEventListener('beforeunload', () => {
    syncRuntimeToStorage().catch((error) => console.warn('Save sync failed', error));
  });
  document.addEventListener('freeze', () => {
    syncRuntimeToStorage().catch((error) => console.warn('Save sync failed', error));
  });
}

if (typeof window !== 'undefined') {
  window.addEventListener('error', (event) => {
    setStatus(`Unhandled error: ${event.message}`, 'error');
  });
  window.addEventListener('unhandledrejection', (event) => {
    setStatus(`Unhandled promise rejection: ${event.reason?.message ?? event.reason}`, 'error');
  });
  init().catch((error) => {
    console.error(error);
    setStatus(error.message, 'error');
    setImportMessage(error.message, 'error');
  });
}
