// Pre-load MPQ files from the server directory into Emscripten virtual filesystem
Module['preRun'] = Module['preRun'] || [];

// SDL3_GetPrefPath("diasurgical", "devilution") writes beneath
// /libsdl/diasurgical. Keep the short-lived /libsdl/preferences layout as a
// migration source only.
// The queue deliberately waits for FS.syncfs's callback: starting a sync is not an
// acknowledgment that the generation is durable.
var DvlBrowserPersistence = (function() {
  var legacyRoot = '/libsdl/preferences';
  var preferenceRoot = '/libsdl/diasurgical';
  var queue = Promise.resolve();
  var dirtyGeneration = 0;
  var durableGeneration = 0;
  var flushScheduled = false;
  var restoreResult;
  var restored = false;
  var lastError = null;
  var lastSyncedAt = null;

  function mkdirSafe(path) {
    try {
      var stat = FS.stat(path);
      if (!FS.isDir(stat.mode)) throw new Error('not a directory: ' + path);
    } catch (e) {
      FS.mkdir(path);
    }
  }

  function sync( populate ) {
    return new Promise(function(resolve, reject) {
      FS.syncfs(populate, function(error) {
        lastError = error ? String(error.message || error) : null;
        if (!error && !populate) lastSyncedAt = Date.now();
        error ? reject(error) : resolve();
      });
    });
  }

  function readLegacyFiles() {
    var source = legacyRoot + '/devilution';
    var files = [];
    try { FS.stat(source); } catch (e) { return files; }
    FS.readdir(source).filter(function(name) { return name !== '.' && name !== '..'; }).forEach(function(name) {
      files.push({ name: name, data: FS.readFile(source + '/' + name) });
    });
    return files;
  }

  function writeMigratedFiles(files) {
    var target = preferenceRoot + '/devilution';
    if (!files.length) return Promise.resolve(false);
    try { FS.stat(target); } catch (e) { mkdirSafe(target); }
    files.forEach(function(file) {
      var targetFile = target + '/' + file.name;
      try { FS.stat(targetFile); } catch (e) { FS.writeFile(targetFile, file.data); }
    });
    return sync(false).then(function() { dirtyGeneration = durableGeneration = 1; return true; });
  }

  function restore() {
    if (restoreResult) return restoreResult;
    restoreResult = Promise.resolve().then(function() {
      mkdirSafe('/libsdl');
      // Open the legacy database only for migration, then unmount it before the
      // SDL3 preference database is opened. There is never a double live mount.
      mkdirSafe(legacyRoot);
      FS.mount(IDBFS, {}, legacyRoot);
      return sync(true);
    }).then(function() {
      var legacyFiles = readLegacyFiles();
      FS.unmount(legacyRoot);
      mkdirSafe(preferenceRoot);
      FS.mount(IDBFS, {}, preferenceRoot);
      return sync(true).then(function() { return writeMigratedFiles(legacyFiles); });
    }).then(function() {
      restored = true;
      console.log('SDL3 preference store restored at ' + preferenceRoot);
      return { path: preferenceRoot, generation: durableGeneration, readOnly: false };
    }).catch(function(error) {
      try { FS.unmount(legacyRoot); } catch (ignored) {}
      console.error('Browser preference restore failed; saves are not durable:', error);
      return { path: preferenceRoot, generation: durableGeneration, readOnly: true, error: error };
    });
    return restoreResult;
  }

  function markDirty() {
    dirtyGeneration++;
    if (!flushScheduled) {
      flushScheduled = true;
      queue = queue.catch(function() { /* allow retry after a failed write */ }).then(function() {
        flushScheduled = false;
        if (dirtyGeneration === durableGeneration) return;
        var generation = dirtyGeneration;
        return sync(false).then(function() {
          // The callback is the durability acknowledgment for this generation.
          durableGeneration = generation;
          if (dirtyGeneration !== durableGeneration) markDirty();
        });
      });
    }
    return queue;
  }

  return { restore: restore, markDirty: markDirty, flush: function(callback) { markDirty().then(function() { callback(null); }, callback); }, getPath: function() { return preferenceRoot; },
    getState: function() { return { dirtyGeneration: dirtyGeneration, durableGeneration: durableGeneration, restored: restored, error: lastError, lastSyncedAt: lastSyncedAt }; } };
})();
if (typeof window !== 'undefined') window.DvlBrowserPersistence = DvlBrowserPersistence;

Module['preRun'].push(function() {
  Module.addRunDependency('syncfs');
  DvlBrowserPersistence.restore().then(function() { Module.removeRunDependency('syncfs'); });
});

// Load MPQ files from the server directory
Module['preRun'].push(function() {
  // List of MPQ files to try loading (in priority order)
  var mpqFiles = [
    'diabdat.mpq',
    'spawn.mpq',
  ];

  // Create a promise-based loading system
  var loadPromises = mpqFiles.map(function(filename) {
    return new Promise(function(resolve) {
      fetch(filename)
        .then(function(response) {
          if (response.ok) {
            return response.arrayBuffer();
          }
          throw new Error('File not found');
        })
        .then(function(data) {
          console.log('Loading ' + filename + ' into virtual filesystem...');
          FS.writeFile('/' + filename, new Uint8Array(data));
          console.log('Successfully loaded ' + filename);
          resolve();
        })
        .catch(function() {
          // File doesn't exist, skip silently
          resolve();
        });
    });
  });

  // Wait for all MPQ files to load before continuing
  Module.addRunDependency('loadMPQs');
  Promise.all(loadPromises).then(function() {
    Module.removeRunDependency('loadMPQs');
  });
});

// Save requests are serialized and coalesced by the persistence owner. A request
// made while a sync is active increments the dirty generation and is flushed next.
Module['saveToIndexedDB'] = function() {
  return DvlBrowserPersistence.markDirty().catch(function(error) {
    console.error('Error persisting saves to IndexedDB:', error);
    throw error;
  });
};

Module['postRun'] = Module['postRun'] || [];
Module['postRun'].push(function() {
  setInterval(function() {
    DvlBrowserPersistence.markDirty().catch(function(error) {
      console.error('Auto-sync error:', error);
    });
  }, 30000);

  // Supplementary best-effort flush only; normal saves do not depend on unload.
  window.addEventListener('pagehide', function() {
    DvlBrowserPersistence.markDirty().catch(function(error) {
      console.error('Error syncing on page hide:', error);
    });
  });
});
