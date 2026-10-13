// File Manager functionality
(function() {
	const modal = document.getElementById('fileManagerModal');
	const fileManagerBtn = document.getElementById('fileManagerBtn');
	const closeModalBtn = document.getElementById('closeModal');
	const dropZone = document.getElementById('dropZone');
	const fileInput = document.getElementById('fileInput');
	const browseBtn = document.getElementById('browseBtn');
	const resetSettingsBtn = document.getElementById('resetSettingsBtn');
	const mpqFilesList = document.getElementById('mpqFilesList');

	// Backups contain only save namespaces, never paths or game archives.
	const saveStatus = document.getElementById('heroSaveStatus');
	const saveList = document.getElementById('heroSaveList');
	const backupBtn = document.getElementById('backupHeroSaves');
	const restoreBtn = document.getElementById('restoreHeroSaves');
	const backupInput = document.getElementById('heroBackupInput');
	const SAVE_NAME = /^(?:(?:single|multi|spawn|share)_\d+|stash(?:_spawn)?)\.[a-zA-Z0-9_-]{1,32}$/;
	const MAX_BACKUP = 128 * 1024 * 1024;
	let saveBusy = false;
	function saveReady() {
		return typeof FS !== 'undefined' && typeof DvlBrowserPersistence !== 'undefined' && DvlBrowserPersistence.getState().restored;
	}
	function saveFiles() {
		return FS.readdir(prefRoot()).filter(name => SAVE_NAME.test(name) && FS.isFile(FS.stat(prefRoot() + '/' + name).mode)).sort();
	}
	function refreshSaveStatus() {
		const ready = saveReady();
		backupBtn.disabled = !ready || saveBusy;
		restoreBtn.disabled = !ready || saveBusy || !!(window.launch && window.launch.started);
		if (!ready) { const state = typeof DvlBrowserPersistence !== 'undefined' ? DvlBrowserPersistence.getState() : {}; saveStatus.textContent = state.error ? 'Browser save storage failed: ' + state.error : 'Save storage is not ready. Existing saves have not been confirmed.'; return; }
		const state = DvlBrowserPersistence.getState();
		saveStatus.textContent = state.error ? 'Browser save failed: ' + state.error :
			state.dirtyGeneration > state.durableGeneration ? 'Saving to browser storage...' :
			state.lastSyncedAt ? 'Browser storage confirmed at ' + new Date(state.lastSyncedAt).toLocaleTimeString() + '. This reflects files written by the game.' : 'Saved files loaded from browser storage.';
		try { const files = saveFiles(); saveList.textContent = files.length ? files.join(', ') : 'No hero or stash files found.'; }
		catch (error) { saveList.textContent = 'Could not list save files: ' + error.message; }
	}
	function persistSaves() {
		return new Promise((resolve, reject) => DvlBrowserPersistence.flush(error => error ? reject(error) : resolve()));
	}
	async function digest(data) {
		return Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', data)), b => b.toString(16).padStart(2, '0')).join('');
	}
	function encodeBytes(data) {
		let text = '';
		for (let offset = 0; offset < data.length; offset += 8192) text += String.fromCharCode(...data.subarray(offset, offset + 8192));
		return btoa(text);
	}
	function downloadBackup(document) {
		const url = URL.createObjectURL(new Blob([JSON.stringify(document)], { type: 'application/json' }));
		const link = window.document.createElement('a');
		link.href = url; link.download = 'damnationx-saves-' + new Date().toISOString().replace(/[:.]/g, '-') + '.json';
		link.click(); setTimeout(() => URL.revokeObjectURL(url), 30000);
	}
	async function makeBackup() {
		const files = [];
		for (const name of saveFiles()) {
			const bytes = FS.readFile(prefRoot() + '/' + name);
			files.push({ name, size: bytes.length, sha256: await digest(bytes), data: encodeBytes(bytes) });
		}
		return { format: 'damnationx-hero-backup', version: 1, createdAt: new Date().toISOString(), files };
	}
	backupBtn.addEventListener('click', async () => {
		if (!saveReady() || saveBusy) return;
		saveBusy = true; refreshSaveStatus();
		try {
			// Preserve a downloadable snapshot even if durable storage currently fails.
			const backup = await makeBackup();
			if (!backup.files.length) throw new Error('No hero or stash files were found to back up.');
			downloadBackup(backup);
			try { await persistSaves(); } catch (error) { alert('Backup downloaded, but browser storage failed: ' + error.message); }
		} catch (error) { alert('Backup failed: ' + error.message); }
		finally { saveBusy = false; refreshSaveStatus(); }
	});
	restoreBtn.addEventListener('click', () => backupInput.click());
	backupInput.addEventListener('change', async () => {
		const file = backupInput.files[0]; backupInput.value = '';
		if (!file || !saveReady() || saveBusy) return;
		if (window.launch && window.launch.started) { alert('Leave the game and reload to the launcher before restoring.'); return; }
		saveBusy = true; refreshSaveStatus();
		const originals = new Map(); let changed = false;
		try {
			if (file.size > MAX_BACKUP) throw new Error('Backup exceeds the 128 MB limit.');
			const backup = JSON.parse(await file.text());
			if (backup.format !== 'damnationx-hero-backup' || backup.version !== 1 || !Array.isArray(backup.files) || !backup.files.length || backup.files.length > 1000) throw new Error('Not a supported hero backup.');
			const names = new Set(); const incoming = []; let total = 0;
			for (const entry of backup.files) {
				if (!entry || typeof entry.name !== 'string' || !SAVE_NAME.test(entry.name) || names.has(entry.name) || typeof entry.data !== 'string' || !/^[a-f0-9]{64}$/.test(entry.sha256) || !Number.isSafeInteger(entry.size) || entry.size <= 0) throw new Error('Backup contains an invalid save entry.');
				names.add(entry.name); total += entry.size;
				if (total > MAX_BACKUP || entry.data.length > MAX_BACKUP * 1.4) throw new Error('Backup is too large.');
				const raw = atob(entry.data); const bytes = Uint8Array.from(raw, c => c.charCodeAt(0));
				if (bytes.length !== entry.size || await digest(bytes) !== entry.sha256) throw new Error('Backup checksum failed for ' + entry.name + '.');
				incoming.push({ name: entry.name, bytes });
			}
			const existing = new Set(saveFiles());
			const replaced = incoming.filter(entry => existing.has(entry.name)).map(entry => entry.name);
			if (!confirm('Restore ' + incoming.length + ' save files?' + (replaced.length ? '\nReplace existing files: ' + replaced.join(', ') + '\nA backup of your current saves will download first.' : '') + '\nOther saves will be kept.')) return;
			if (window.launch && window.launch.started) throw new Error('The game started during restore. Reload to the launcher first.');
			if (replaced.length) downloadBackup(await makeBackup());
			if (window.launch && window.launch.started) throw new Error('The game started during restore. Reload to the launcher first.');
			for (const entry of incoming) {
				const path = prefRoot() + '/' + entry.name;
				originals.set(entry.name, existing.has(entry.name) ? FS.readFile(path) : null);
			}
			changed = true;
			for (const entry of incoming) FS.writeFile(prefRoot() + '/' + entry.name, entry.bytes);
			await persistSaves();
			alert('Restore confirmed in browser storage. Reloading to load your heroes.');
			location.reload();
		} catch (error) {
			if (changed) {
				for (const [name, bytes] of originals) { const path = prefRoot() + '/' + name; if (bytes === null) { try { FS.unlink(path); } catch (_) {} } else FS.writeFile(path, bytes); }
				try { await persistSaves(); } catch (_) { alert('Browser storage could not confirm rollback. Keep your downloaded backup and avoid starting a game until storage works.'); }
			}
			alert('Restore failed: ' + error.message);
		} finally { saveBusy = false; refreshSaveStatus(); }
	});
	setInterval(() => { if (modal.classList.contains('show')) refreshSaveStatus(); }, 1000);

	// Game data archives live in the preference root; everything else is a mod pack and
	// belongs in mods/, which is where the engine discovers packed mods.
	const CORE_ARCHIVES = new Set(['diabdat', 'spawn', 'hellfire', 'hfmonk', 'hfmusic', 'hfvoice', 'hfbard', 'hfbarb', 'devilutionx', 'devx', 'fonts']);
	function isCoreArchive(filename) {
		const stem = filename.toLowerCase().replace(/\.mpq$/, '');
		return CORE_ARCHIVES.has(stem) || /^[a-z]{2}(_[a-z]{2})?$/.test(stem); // language packs
	}
	function prefRoot() {
		return DvlBrowserPersistence.getPath() + '/devilution';
	}
	function archivePath(filename) {
		return isCoreArchive(filename) ? prefRoot() + '/' + filename.toLowerCase() : prefRoot() + '/mods/' + filename;
	}
	function mkdirSafe(path) {
		try { FS.mkdir(path); } catch (e) { /* exists */ }
	}
	// Enable a freshly uploaded mod pack in diablo.ini so it is active after the reload.
	function enableModInIni(filename) {
		const name = filename.replace(/\.mpq$/i, '');
		const iniPath = prefRoot() + '/diablo.ini';
		let ini = '';
		try { ini = FS.readFile(iniPath, { encoding: 'utf8' }); } catch (e) { /* first run */ }
		const line = name + '=1';
		const existing = new RegExp('^' + name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + '=.*$', 'm');
		if (/^\[Mods\]/m.test(ini)) {
			ini = existing.test(ini) ? ini.replace(existing, line) : ini.replace(/^\[Mods\][ \t]*\r?\n/m, m => m + line + '\n');
		} else {
			ini += (ini && !ini.endsWith('\n') ? '\n' : '') + '[Mods]\n' + line + '\n';
		}
		FS.writeFile(iniPath, ini);
	}
	// [{ label, path }] for every archive in the root and in mods/.
	function listArchives() {
		const entries = [];
		const scan = (dir, prefix) => {
			let names;
			try { names = FS.readdir(dir); } catch (e) { return; }
			names.filter(f => f.toLowerCase().endsWith('.mpq')).sort().forEach(f => entries.push({ label: prefix + f, path: dir + '/' + f }));
		};
		scan(prefRoot(), '');
		scan(prefRoot() + '/mods', 'mods/');
		return entries;
	}

	// Open/close modal
	fileManagerBtn.addEventListener('click', () => {
		modal.classList.add('show');
		refreshFileList();
		refreshSaveStatus();
	});

	closeModalBtn.addEventListener('click', () => {
		modal.classList.remove('show');
	});

	modal.addEventListener('click', (e) => {
		if (e.target === modal) {
			modal.classList.remove('show');
		}
	});

	// Browse button
	browseBtn.addEventListener('click', () => {
		fileInput.click();
	});

	// Drag and drop
	dropZone.addEventListener('click', () => {
		fileInput.click();
	});

	dropZone.addEventListener('dragover', (e) => {
		e.preventDefault();
		dropZone.classList.add('dragover');
	});

	dropZone.addEventListener('dragleave', () => {
		dropZone.classList.remove('dragover');
	});

	dropZone.addEventListener('drop', (e) => {
		e.preventDefault();
		dropZone.classList.remove('dragover');
		handleFiles(e.dataTransfer.files);
	});

	fileInput.addEventListener('change', (e) => {
		handleFiles(e.target.files);
	});

	// Handle file upload
	// Handle file upload. Reimport is asynchronous, but one request owns the
	// transaction: the native catalog validator is the authority for activation.
	let reimportSequence = 0;
	function reportReimport(result) {
		const detail = result && result.diagnostics ? result.diagnostics.map(d => d.message || d.code).join('\\n') : '';
		const message = result && result.accepted ? 'Mod pack ready; reload is staged at a frame boundary.' : 'Mod pack rejected; the previous catalog remains active.';
		alert(detail ? message + '\\n\\n' + detail : message);
	}

	function requestCatalogReimport(paths) {
		const requestId = ++reimportSequence;
		const payload = JSON.stringify({ requestId, paths, source: 'browser-file-manager' });
		if (typeof Module.dvl_mod_reimport !== 'function')
			return Promise.reject(new Error('In-client catalog reimport is unavailable.'));
		return Promise.resolve(Module.dvl_mod_reimport(payload)).then(raw => {
			const result = typeof raw === 'string' ? JSON.parse(raw) : raw;
			if (!result || result.requestId !== requestId)
				throw new Error('Catalog reimport response did not match its request.');
			reportReimport(result);
			return result;
		});
	}

	function handleFiles(files) {
		if (!files || files.length === 0) return;

		// Wait for Module and FS to be ready
		if (typeof Module === 'undefined' || typeof FS === 'undefined') {
			alert('Game is still loading. Please wait and try again.');
			return;
		}

		const mpqFiles = Array.from(files).filter(f =>
			f.name.toLowerCase().endsWith('.mpq')
		);

		if (mpqFiles.length === 0) {
			alert('Please select MPQ files only.');
			return;
		}

		let processed = 0;
		mpqFiles.forEach(file => {
			const reader = new FileReader();
			reader.onload = function(e) {
				try {
					const data = new Uint8Array(e.target.result);
					// Game data goes to the preference root; mod packs go to mods/ where the
					// engine discovers them.
					const path = archivePath(file.name);
					mkdirSafe(prefRoot());
					if (!isCoreArchive(file.name)) {
						mkdirSafe(prefRoot() + '/mods');
					}

					// Write file to IDBFS-backed directory
					FS.writeFile(path, data);
					if (!isCoreArchive(file.name)) {
						enableModInIni(file.name);
					}
					console.log('Uploaded:', path, '(' + formatBytes(file.size) + ')');

					processed++;
					if (processed === mpqFiles.length) {
						// Sync to IndexedDB
						FS.syncfs(false, function(err) {
							if (err) {
								console.error('Error syncing files:', err);
								alert('Error saving files. Check console.');
							} else {
								if (window.launch && !window.launch.started) {
									location.reload();
									return;
								}
								requestCatalogReimport(mpqFiles.map(file => file.name)).catch(error => {
								console.error('Error reimporting catalog:', error);
								alert('Error reimporting pack: ' + error.message);
							});
								setTimeout(() => location.reload(), 500);
							}
						});
					}
				} catch (err) {
					console.error('Error writing file:', err);
					alert('Error uploading file: ' + file.name);
				}
			};
			reader.readAsArrayBuffer(file);
		});
	}

	// Refresh file list
	function refreshFileList() {
		if (typeof Module === 'undefined' || typeof FS === 'undefined') {
			mpqFilesList.innerHTML = '<p class="info-text">Game is loading...</p>';
			return;
		}

		try {
			const archives = listArchives();
			if (archives.length === 0) {
				mpqFilesList.innerHTML = '<p class="info-text">No MPQ files found.</p>';
				return;
			}

			mpqFilesList.innerHTML = '';
			archives.forEach(({ label, path }) => {
				const stat = FS.stat(path);

				const item = document.createElement('div');
				item.className = 'file-item';

				const nameSpan = document.createElement('span');
				nameSpan.className = 'file-item-name';
				nameSpan.textContent = label;

				const sizeSpan = document.createElement('span');
				sizeSpan.className = 'file-item-size';
				sizeSpan.textContent = formatBytes(stat.size);

				const deleteBtn = document.createElement('button');
				deleteBtn.className = 'btn btn-delete';
				deleteBtn.textContent = 'Delete';
				deleteBtn.addEventListener('click', () => window.deleteFile(label));

				item.appendChild(nameSpan);
				item.appendChild(sizeSpan);
				item.appendChild(deleteBtn);
				mpqFilesList.appendChild(item);
			});
		} catch (err) {
			console.error('Error reading files:', err);
			mpqFilesList.innerHTML = '<p class="info-text">Error reading files.</p>';
		}
	}

	// Delete file
	window.deleteFile = function(label) {
		if (!confirm('Delete ' + label + '? This will reload the game.')) {
			return;
		}

		try {
			// `label` is the listed name: either "<file>.mpq" (root) or "mods/<file>.mpq".
			FS.unlink(prefRoot() + '/' + label);

			// Sync deletion to IndexedDB
			FS.syncfs(false, function(err) {
				if (err) {
					console.error('Error syncing deletion:', err);
					alert('Error deleting file. Check console.');
				} else {
					alert('File deleted! Reloading game...');
					setTimeout(() => location.reload(), 500);
				}
			});
		} catch (err) {
			console.error('Error deleting file:', err);
			alert('Error deleting file: ' + label);
		}
	};

	// Reset settings
	resetSettingsBtn.addEventListener('click', () => {
		if (!confirm('Reset game settings? This will delete diablo.ini but keep your saves. The game will reload.')) {
			return;
		}

		try {
			const iniPath = DvlBrowserPersistence.getPath() + '/devilution/diablo.ini';

			// Check if file exists
			try {
				FS.stat(iniPath);
				// File exists, delete it
				FS.unlink(iniPath);
				console.log('Deleted diablo.ini');
			} catch (e) {
				// File doesn't exist, that's fine
				console.log('diablo.ini not found (already reset)');
			}

			// Sync to IndexedDB
			FS.syncfs(false, function(err) {
				if (err) {
					console.error('Error syncing settings reset:', err);
					alert('Error resetting settings. Check console.');
				} else {
					alert('Settings reset! Reloading game...');
					setTimeout(() => location.reload(), 500);
				}
			});
		} catch (err) {
			console.error('Error resetting settings:', err);
			alert('Error resetting settings.');
		}
	});

	// Helper function
	function formatBytes(bytes) {
		if (bytes === 0) return '0 Bytes';
		const k = 1024;
		const sizes = ['Bytes', 'KB', 'MB', 'GB'];
		const i = Math.floor(Math.log(bytes) / Math.log(k));
		return Math.round(bytes / Math.pow(k, i) * 100) / 100 + ' ' + sizes[i];
	}
})();
