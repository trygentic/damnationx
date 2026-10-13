// "Diablo II Sounds" dialog: imports the sounds of the user's Diablo II (MPQ archives) or
// Diablo II: Resurrected (CASC storage) as sound pack mods, like the desktop importer does
// (Source/d2sound/). The game can't read the install folder in the browser, so:
//   - the user chooses the game's folder (File System Access API, or <input webkitdirectory>)
//     or, for Diablo II, its MPQ files;
//   - the chosen File objects are passed to a worker (d2sound-importer.js) that reads them in
//     place with WORKERFS: only the needed byte ranges are read, nothing is copied (a Diablo II:
//     Resurrected storage is tens of GB);
//   - the worker posts each imported sound back and this script writes it into the persistent
//     mod folders (<pref>/mods/{d2sounds,d2r-sounds-classic,d2r-sounds-advanced}), then syncs
//     IndexedDB. The pack is then selected in Settings > Audio > Sound Pack.
// The game opens this dialog (window.DvlD2Sounds.open) when a pack that isn't imported is selected.
(function() {
	'use strict';

	var PACKS = {
		d2: [{ mod: 'd2sounds', name: 'Diablo II' }],
		d2r: [
			{ mod: 'd2r-sounds-classic', name: 'Diablo II Resurrected (Classic)' },
			{ mod: 'd2r-sounds-advanced', name: 'Diablo II Resurrected (Advanced)' },
		],
	};
	var ALL_PACKS = PACKS.d2.concat(PACKS.d2r);
	// The archives the importer reads (Diablo2SoundArchives in Source/d2sound/d2sound_core.hpp).
	var D2_ARCHIVES = ['patch_d2.mpq', 'd2exp.mpq', 'd2xtalk.mpq', 'd2sfx.mpq', 'd2speech.mpq', 'd2char.mpq', 'd2data.mpq'];
	var SOUND_MAPS = ['txtdata/sound/d2_sfx_map.tsv', 'txtdata/sound/d2_speech_map.tsv'];
	var SOURCE_NAMES = { d2: 'Diablo II', d2r: 'Diablo II Resurrected' };
	var MISSING_IMPORTER = 'This build of the game does not include the Diablo II sound importer: it was configured with DEVILUTIONX_D2_SOUND_IMPORT=OFF. Configure it with -DDEVILUTIONX_D2_SOUND_IMPORT=ON and rebuild.';

	var modal = document.getElementById('d2SoundModal');
	var openButton = document.getElementById('d2SoundBtn');
	var closeButton = document.getElementById('d2SoundClose');
	var statusElement = document.getElementById('d2SoundStatus');
	var progressElement = document.getElementById('d2SoundProgress');
	var cancelButton = document.getElementById('d2SoundCancel');
	var packList = document.getElementById('d2SoundPacks');
	var dirInput = document.getElementById('d2SoundDirInput');
	var mpqInput = document.getElementById('d2SoundMpqInput');
	var choiceButtons = Array.prototype.slice.call(modal.querySelectorAll('[data-d2-choose]'));

	var current = null; // the running import: { worker, source }
	var pendingDirSource = null;

	function prefRoot() {
		return DvlBrowserPersistence.getPath() + '/devilution';
	}
	function modsRoot() {
		return prefRoot() + '/mods';
	}
	function exists(path) {
		try { FS.stat(path); return true; } catch (e) { return false; }
	}
	function fsReady() {
		return typeof FS !== 'undefined' && typeof DvlBrowserPersistence !== 'undefined' && exists(prefRoot());
	}
	function isImported(mod) {
		return exists(modsRoot() + '/' + mod + '/manifest.ini');
	}
	function folderSize(path) {
		var total = 0;
		var stat = FS.stat(path);
		if (!FS.isDir(stat.mode)) return stat.size;
		FS.readdir(path).forEach(function(name) {
			if (name !== '.' && name !== '..') total += folderSize(path + '/' + name);
		});
		return total;
	}
	function removeTree(path) {
		var stat = FS.stat(path);
		if (!FS.isDir(stat.mode)) { FS.unlink(path); return; }
		FS.readdir(path).forEach(function(name) {
			if (name !== '.' && name !== '..') removeTree(path + '/' + name);
		});
		FS.rmdir(path);
	}
	function formatBytes(bytes) {
		if (bytes < 1024 * 1024) return Math.round(bytes / 1024) + ' KB';
		return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
	}
	function persist() {
		return DvlBrowserPersistence.markDirty();
	}

	function setStatus(text, isError) {
		statusElement.textContent = text;
		statusElement.className = isError ? 'd2-status d2-error' : 'd2-status';
	}
	function setBusy(busy) {
		choiceButtons.forEach(function(button) { button.disabled = busy; });
		cancelButton.hidden = !busy;
		progressElement.hidden = !busy;
		if (busy) progressElement.value = 0;
	}

	function refreshPacks() {
		if (!fsReady()) {
			packList.innerHTML = '<p class="info-text">Game is loading...</p>';
			return;
		}
		packList.innerHTML = '';
		ALL_PACKS.forEach(function(pack) {
			var item = document.createElement('div');
			item.className = 'file-item';
			item.dataset.mod = pack.mod;
			var name = document.createElement('span');
			name.className = 'file-item-name';
			name.textContent = pack.name;
			var info = document.createElement('span');
			info.className = 'file-item-size';
			var imported = isImported(pack.mod);
			info.textContent = imported ? 'imported, ' + formatBytes(folderSize(modsRoot() + '/' + pack.mod)) : 'not imported';
			item.appendChild(name);
			item.appendChild(info);
			if (exists(modsRoot() + '/' + pack.mod)) {
				var remove = document.createElement('button');
				remove.className = 'btn btn-delete';
				remove.textContent = 'Remove';
				remove.addEventListener('click', function() { removePack(pack); });
				item.appendChild(remove);
			}
			packList.appendChild(item);
		});
	}

	function removePack(pack) {
		if (current) return;
		if (!confirm('Remove the ' + pack.name + ' sound pack? If it is selected in Settings > Audio > Sound Pack, select another pack first.')) return;
		try {
			removeTree(modsRoot() + '/' + pack.mod);
		} catch (error) {
			console.error('Could not remove ' + pack.mod + ':', error);
		}
		persist().then(refreshPacks, function(error) { console.error(error); refreshPacks(); });
	}

	function readSoundMaps() {
		// The maps are game assets (preloaded at /assets in the browser build).
		return SOUND_MAPS.map(function(path) {
			var candidates = ['/assets/' + path, '/' + path];
			for (var i = 0; i < candidates.length; i++) {
				try { return FS.readFile(candidates[i], { encoding: 'utf8' }); } catch (e) { /* next */ }
			}
			throw new Error(MISSING_IMPORTER);
		});
	}
	// Whether the game was built with the importer (DEVILUTIONX_D2_SOUND_IMPORT): only then does it
	// have the sound maps and the Sound Pack option.
	function hasImporter() {
		try { readSoundMaps(); return true; } catch (e) { return false; }
	}

	// [{ path, blob }] -> the files that the importer needs, with paths relative to the game folder.
	function selectFiles(source, entries) {
		if (source === 'd2') {
			return entries.filter(function(entry) {
				return entry.path.indexOf('/') < 0 && D2_ARCHIVES.indexOf(entry.path.toLowerCase()) >= 0;
			});
		}
		return entries.filter(function(entry) {
			var lower = entry.path.toLowerCase();
			return lower === '.build.info' || lower.indexOf('data/') === 0;
		});
	}

	function validate(source, files) {
		if (source === 'd2') {
			if (files.length === 0)
				return 'No Diablo II archives found. Choose the Diablo II folder or its MPQ files (' + D2_ARCHIVES.join(', ') + ').';
			return null;
		}
		var paths = files.map(function(file) { return file.path.toLowerCase(); });
		if (paths.indexOf('.build.info') < 0)
			return 'This is not a Diablo II Resurrected folder: .build.info is missing. Choose the folder that contains D2R.exe and Data'
				+ (window.showDirectoryPicker ? '.' : ' (if it is, this browser hides the .build.info file: use Chrome or Edge).');
		if (!paths.some(function(path) { return /^data\/data\/[^/]+\.idx$/.test(path); }))
			return 'This is not a Diablo II Resurrected folder: Data/data has no .idx files.';
		return null;
	}

	// Walks a FileSystemDirectoryHandle, without descending into folders that the importer doesn't read.
	async function collectFromHandle(source, handle) {
		var entries = [];
		async function walk(dir, prefix) {
			for await (var child of dir.values()) {
				var path = prefix + child.name;
				var lower = path.toLowerCase();
				if (child.kind === 'file') {
					entries.push({ path: path, file: child });
				} else if (source === 'd2r' && (lower === 'data' || lower.indexOf('data/') === 0)) {
					await walk(child, path + '/');
				}
			}
		}
		await walk(handle, '');
		var selected = selectFiles(source, entries);
		for (var i = 0; i < selected.length; i++)
			selected[i] = { path: selected[i].path, blob: await selected[i].file.getFile() };
		return selected;
	}

	function collectFromFileList(source, fileList, fromDirectory) {
		var entries = Array.prototype.map.call(fileList, function(file) {
			var path = file.name;
			if (fromDirectory && file.webkitRelativePath) {
				// "<chosen folder>/sub/file" -> "sub/file"
				path = file.webkitRelativePath.split('/').slice(1).join('/');
			}
			return { path: path, blob: file };
		});
		return selectFiles(source, entries);
	}

	function writeFile(mod, path, data) {
		var parts = path.split('\\').filter(function(part) { return part.length > 0; });
		var dir = modsRoot() + '/' + mod;
		if (parts.length > 1) dir += '/' + parts.slice(0, -1).join('/');
		FS.mkdirTree(dir);
		FS.writeFile(dir + '/' + parts[parts.length - 1], data);
	}

	function describeResult(result) {
		var name = SOURCE_NAMES[result.source];
		switch (result.error) {
		case '': break;
		case 'open-archives-failed': return 'Failed to open the Diablo II archives.';
		case 'open-storage-failed': return 'Failed to open the Diablo II Resurrected data (Data/data, Data/config and .build.info are needed).';
		case 'no-sounds-found': return 'No sounds were found in these files.';
		case 'sound-maps-missing': return 'The sound maps are missing from the game assets.';
		default: return 'The import failed (' + result.error + ').';
		}
		var text = result.source === 'd2'
			? 'Imported ' + result.imported + ' of ' + result.total + ' sounds from ' + name + '.'
			: 'Imported ' + Math.max(result.imported, result.importedAdvanced) + ' of ' + result.total + ' sounds from ' + name + ' (' + result.remastered + ' remastered).';
		if (result.missing.length)
			text += ' ' + result.missing.length + ' sounds were not found and keep their original sound.';
		return text + ' If you chose it in the game\'s Settings, the game uses it once you go back to its main menu. Otherwise select it in Settings > Audio > Sound Pack.';
	}

	function finish(text, isError) {
		var importer = current;
		current = null;
		if (importer) importer.worker.terminate();
		setBusy(false);
		setStatus(text + (isError ? '' : ' Saving...'), isError);
		return persist().then(function() {
			if (!isError) setStatus(text, false);
			refreshPacks();
		}, function(error) {
			console.error('Could not save the sound pack:', error);
			setStatus('The sound pack could not be saved in the browser storage: ' + error, true);
		});
	}

	function startImport(source, files) {
		if (current) return;
		if (!fsReady()) {
			setStatus('The game is still loading. Try again in a moment.', true);
			return;
		}
		var problem = validate(source, files);
		if (problem) {
			setStatus(problem, true);
			return;
		}
		var maps;
		try {
			maps = readSoundMaps();
		} catch (error) {
			setStatus(error.message, true);
			return;
		}
		var worker = new Worker('d2sound-importer.js');
		current = { worker: worker, source: source, files: 0, bytes: 0 };
		var importer = current;
		setBusy(true);
		setStatus('Reading ' + SOURCE_NAMES[source] + ' (' + files.length + ' files)...', false);
		worker.onmessage = function(event) {
			if (current !== importer) return;
			var message = event.data;
			switch (message.type) {
			case 'begin-pack':
				try { FS.unlink(modsRoot() + '/' + message.mod + '/manifest.ini'); } catch (e) { /* not imported yet */ }
				break;
			case 'file':
				try {
					writeFile(message.mod, message.path, message.data);
					importer.files++;
					importer.bytes += message.data.length;
				} catch (error) {
					console.error('Could not write ' + message.mod + '/' + message.path + ':', error);
				}
				break;
			case 'progress':
				progressElement.value = message.progress;
				if (message.progress > 1 && message.progress < 100)
					setStatus('Importing ' + SOURCE_NAMES[source] + ' sounds: ' + message.progress + '% (' + formatBytes(importer.bytes) + ')', false);
				break;
			case 'log':
				(message.level === 'error' ? console.error : message.level === 'warn' ? console.warn : console.log)('[d2sound] ' + message.message);
				break;
			case 'done':
				console.log('[d2sound] result', message.result);
				finish(describeResult(message.result), !message.result.ok);
				break;
			case 'error':
				finish('The import failed: ' + message.message, true);
				break;
			}
		};
		worker.onerror = function(event) {
			event.preventDefault();
			if (current === importer) finish('The import failed: ' + (event.message || 'the importer could not be loaded.'), true);
		};
		worker.postMessage({ type: 'start', source: source, files: files, maps: maps });
	}

	async function chooseFolder(source) {
		if (window.showDirectoryPicker) {
			var handle;
			try {
				handle = await window.showDirectoryPicker({ id: 'dvl-' + source, mode: 'read' });
			} catch (error) {
				if (error && error.name === 'AbortError') return;
				console.warn('showDirectoryPicker failed, using the folder input instead:', error);
				handle = null;
			}
			if (handle) {
				setStatus('Reading the folder...', false);
				var files;
				try {
					files = await collectFromHandle(source, handle);
				} catch (error) {
					setStatus('Could not read the folder: ' + error, true);
					return;
				}
				startImport(source, files);
				return;
			}
		}
		pendingDirSource = source;
		dirInput.value = '';
		dirInput.click();
	}

	choiceButtons.forEach(function(button) {
		button.addEventListener('click', function() {
			var choice = button.getAttribute('data-d2-choose');
			if (choice === 'd2-files') {
				mpqInput.value = '';
				mpqInput.click();
			} else {
				chooseFolder(choice === 'd2r-folder' ? 'd2r' : 'd2');
			}
		});
	});
	dirInput.addEventListener('change', function() {
		if (!pendingDirSource || !dirInput.files.length) return;
		startImport(pendingDirSource, collectFromFileList(pendingDirSource, dirInput.files, true));
	});
	mpqInput.addEventListener('change', function() {
		if (!mpqInput.files.length) return;
		startImport('d2', collectFromFileList('d2', mpqInput.files, false));
	});
	cancelButton.addEventListener('click', function() {
		if (current) finish('The import was cancelled. The pack is not available until it is imported completely.', true);
	});

	function open(source) {
		modal.classList.add('show');
		refreshPacks();
		modal.querySelectorAll('.d2-source').forEach(function(section) {
			section.classList.toggle('d2-highlight', !!source && section.getAttribute('data-source') === source);
		});
		if (!current && fsReady() && !hasImporter()) {
			setStatus(MISSING_IMPORTER, true);
			return;
		}
		if (!current && source)
			setStatus('Choose your ' + SOURCE_NAMES[source] + (source === 'd2' ? ' folder or its MPQ files.' : ' folder.'), false);
	}
	function close() {
		modal.classList.remove('show');
	}
	openButton.addEventListener('click', function() { open(null); });
	closeButton.addEventListener('click', close);
	modal.addEventListener('click', function(event) { if (event.target === modal) close(); });

	window.DvlD2Sounds = {
		open: open,
		// For automated tests: imports from in-memory or URL-backed files ([{ path, blob } | { path, url, size }]).
		importFiles: function(source, files) { open(source); startImport(source, selectFiles(source, files)); },
		isImported: function(mod) { return fsReady() && isImported(mod); },
		isBusy: function() { return !!current; },
		status: function() { return statusElement.textContent; },
	};
})();
