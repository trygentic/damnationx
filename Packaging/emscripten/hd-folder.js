// Use a local HD asset pack without hosting it: in the File Manager (the launcher's "Game files…")
// the player chooses their pack folder once (the "hd" folder, or the folder that contains it), and
// requests the engine makes for hd/... are answered from that folder, one file at a time, as the
// game asks for them. Nothing is copied or uploaded.
// With the File System Access API (Chrome, Edge) the folder is remembered across reloads. Other
// browsers (Brave by default, Firefox, Safari) use a folder <input> instead: same on-demand reads,
// but the folder has to be chosen again after each reload.
//
// The folder handle is kept in IndexedDB; after a reload Chrome usually asks once more for
// permission, which needs a click ("Reconnect HD Folder"). Choose or reconnect before pressing Play:
// the engine reads the hd* URL flags, which this sets, when it starts.
(function () {
	'use strict';
	var FLAGS = ['hdFloor', 'hdSprites', 'hdTiles', 'hdUi'];
	var DB = 'damnationx-hd', STORE = 'handles', KEY = 'hd';
	var choose = document.getElementById('hdChooseBtn');
	var forget = document.getElementById('hdForgetBtn');
	var status = document.getElementById('hdStatus');
	var hdDir = null; // FileSystemDirectoryHandle of the folder holding sprites.txt
	var hdFiles = null; // fallback: Map of path inside the hd folder -> File, from a folder <input>
	var stored = null; // handle restored from IndexedDB that still needs permission
	var added = []; // flags this script put in the URL, removed again by "Stop Using HD Folder"
	var base = new URL('hd/', location.href).pathname;

	if (!choose || !status) return;
	var canRemember = !!window.showDirectoryPicker;

	function db() {
		return new Promise(function (resolve, reject) {
			var request = indexedDB.open(DB, 1);
			request.onupgradeneeded = function () { request.result.createObjectStore(STORE); };
			request.onsuccess = function () { resolve(request.result); };
			request.onerror = function () { reject(request.error); };
		});
	}
	function idb(mode, action) {
		return db().then(function (database) {
			return new Promise(function (resolve, reject) {
				var request = action(database.transaction(STORE, mode).objectStore(STORE));
				request.onsuccess = function () { resolve(request.result); };
				request.onerror = function () { reject(request.error); };
			});
		});
	}

	// Accept the hd folder itself or its parent (the unzipped pack folder).
	async function hdFolderIn(dir) {
		try { await dir.getFileHandle('sprites.txt'); return dir; } catch (e) {}
		try { var sub = await dir.getDirectoryHandle('hd'); await sub.getFileHandle('sprites.txt'); return sub; } catch (e) {}
		return null;
	}

	function setFlags(on) {
		var url = new URL(location.href);
		if (on) {
			FLAGS.forEach(function (flag) { if (!url.searchParams.has(flag)) { url.searchParams.set(flag, '1'); added.push(flag); } });
		} else {
			added.forEach(function (flag) { url.searchParams.delete(flag); });
			added = [];
		}
		history.replaceState(history.state, '', url);
	}

	function started() { return !!(window.Module && Module.calledRun); }

	function use(dir, note) {
		if (canRemember) hdDir = dir;
		stored = null;
		setFlags(true);
		status.textContent = 'Using HD folder "' + dir.name + '"' + (note || '') + '.' + (started() ? ' Reload the page to apply it.' : '');
		choose.textContent = 'Choose Another HD Folder…';
		if (forget) forget.hidden = false;
	}

	function pickWithInput() {
		var input = document.createElement('input');
		input.type = 'file';
		input.webkitdirectory = true;
		input.addEventListener('change', function () {
			var files = Array.from(input.files || []);
			if (!files.length) return;
			// webkitRelativePath is "<chosen>/...": find the prefix of the folder that holds sprites.txt
			var index = files.find(function (f) { return /(^|\/)sprites\.txt$/.test(f.webkitRelativePath) && f.webkitRelativePath.split('/').length <= 3; });
			if (!index) {
				status.textContent = '"' + files[0].webkitRelativePath.split('/')[0] + '" has no HD pack. Choose the "hd" folder (it contains sprites.txt) or the folder that contains it.';
				return;
			}
			var prefix = index.webkitRelativePath.slice(0, -'sprites.txt'.length);
			hdFiles = new Map();
			files.forEach(function (f) { if (f.webkitRelativePath.startsWith(prefix)) hdFiles.set(f.webkitRelativePath.slice(prefix.length), f); });
			use({ name: prefix.replace(/\/$/, '').split('/').pop() || 'hd' }, ' (choose it again after a reload)');
		});
		input.click();
	}

	async function pick() {
		if (!canRemember) return pickWithInput();
		var chosen;
		try {
			chosen = await window.showDirectoryPicker({ id: 'damnationx-hd', mode: 'read' });
		} catch (e) {
			return; // cancelled
		}
		var dir = await hdFolderIn(chosen);
		if (!dir) {
			status.textContent = '"' + chosen.name + '" has no HD pack. Choose the "hd" folder (it contains sprites.txt) or the folder that contains it.';
			return;
		}
		await idb('readwrite', function (s) { return s.put(dir, KEY); }).catch(function () {});
		use(dir);
	}

	choose.addEventListener('click', async function () {
		if (stored && await stored.requestPermission({ mode: 'read' }) === 'granted') return use(stored);
		return pick();
	});

	if (forget) {
		forget.addEventListener('click', async function () {
			await idb('readwrite', function (s) { return s.delete(KEY); }).catch(function () {});
			hdDir = stored = hdFiles = null;
			setFlags(false);
			status.textContent = 'No HD folder selected.' + (started() ? ' Reload the page to apply it.' : '');
			choose.textContent = 'Choose HD Folder…';
			forget.hidden = true;
		});
	}

	if (canRemember) idb('readonly', function (s) { return s.get(KEY); }).then(async function (handle) {
		if (!handle) return;
		if (await handle.queryPermission({ mode: 'read' }) === 'granted') return use(handle);
		stored = handle;
		status.textContent = 'HD folder "' + handle.name + '" needs permission again.';
		choose.textContent = 'Reconnect HD Folder';
		if (forget) forget.hidden = false;
	}).catch(function () {});

	var TYPES = { png: 'image/png', webp: 'image/webp', txt: 'text/plain; charset=utf-8', json: 'application/json' };
	function fileResponse(file, name) {
		var ext = name.split('.').pop().toLowerCase();
		return new Response(file, { status: 200, headers: { 'Content-Type': TYPES[ext] || 'application/octet-stream', 'Content-Length': String(file.size) } });
	}
	var innerFetch = window.fetch;
	window.fetch = async function (input, options) {
		var url = new URL(typeof input === 'string' || input instanceof URL ? input : input.url, location.href);
		if (!(hdDir || hdFiles) || url.origin !== location.origin || !url.pathname.startsWith(base)) return innerFetch(input, options);
		var parts = url.pathname.slice(base.length).split('/').map(decodeURIComponent);
		try {
			if (hdFiles) {
				var found = hdFiles.get(parts.join('/'));
				if (!found) throw new Error('missing');
				return fileResponse(found, parts[parts.length - 1]);
			}
			var dir = hdDir;
			for (var i = 0; i < parts.length - 1; i++) dir = await dir.getDirectoryHandle(parts[i]);
			return fileResponse(await (await dir.getFileHandle(parts[parts.length - 1])).getFile(), parts[parts.length - 1]);
		} catch (e) {
			return new Response('', { status: 404, statusText: 'Not in the local HD folder' });
		}
	};
})();
