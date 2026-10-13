// The Diablo II sound import worker (see Source/d2sound/d2sound_web_worker.cpp): this file is
// prepended to d2sound-importer.js, which the page starts as a Web Worker (see d2sound-ui.js).
//
// Messages from the page:
//   { type: 'start', source: 'd2' | 'd2r', files: [{ path, blob }], maps: [tsv, ...] }
//     `path` is relative to the game's folder ('d2sfx.mpq', 'Data/data/data.000', ...).
//     For automated tests, an entry can be { path, url, size } instead: it is read with
//     synchronous range requests (the server must support HTTP Range requests).
// Messages to the page:
//   { type: 'begin-pack', mod }               remove the pack's manifest.ini (written again last)
//   { type: 'file', mod, path, data }         a file of a pack (`path` uses '\' separators)
//   { type: 'progress', progress }            0 to 100
//   { type: 'log', level, message }
//   { type: 'done', result }                  see d2w_result_json()
//   { type: 'error', message }
Module['noInitialRun'] = true;
Module['print'] = function(text) { postMessage({ type: 'log', level: 'info', message: String(text) }); };
Module['printErr'] = function(text) { postMessage({ type: 'log', level: 'warn', message: String(text) }); };

var d2wRuntimeReady = new Promise(function(resolve) { Module['onRuntimeInitialized'] = resolve; });

// A Blob-like object for WORKERFS that reads byte ranges of a URL synchronously (tests only).
function d2wUrlContents(url, size) {
	return {
		size: size,
		slice: function(start, end) {
			end = Math.min(end, size);
			if (end <= start) return new Blob([]);
			var xhr = new XMLHttpRequest();
			xhr.open('GET', url, false);
			xhr.responseType = 'arraybuffer';
			xhr.setRequestHeader('Range', 'bytes=' + start + '-' + (end - 1));
			xhr.send(null);
			if (xhr.status !== 206 && !(xhr.status === 200 && start === 0 && end === size))
				throw new Error('Range request failed for ' + url + ': HTTP ' + xhr.status);
			return new Blob([xhr.response]);
		}
	};
}

function d2wStart(message) {
	FS.mkdirTree('/src');
	FS.mount(WORKERFS, {
		blobs: message.files.map(function(file) {
			return { name: file.path, data: file.blob || d2wUrlContents(file.url, file.size) };
		})
	}, '/src');
	FS.mkdirTree('/maps');
	var mapPaths = message.maps.map(function(tsv, i) {
		var path = '/maps/' + i + '.tsv';
		FS.writeFile(path, tsv);
		return path;
	});
	ccall('d2w_begin', 'number', ['number', 'string', 'string'], [message.source === 'd2r' ? 1 : 0, '/src', mapPaths.join('\n')]);
	var progress = 0;
	while (progress < 100) {
		progress = ccall('d2w_step', 'number', ['number'], [200]);
		postMessage({ type: 'progress', progress: progress });
	}
	var result = JSON.parse(UTF8ToString(ccall('d2w_result_json', 'number', [], [])));
	postMessage({ type: 'done', result: result });
}

self.onmessage = function(event) {
	var message = event.data;
	if (!message || message.type !== 'start') return;
	d2wRuntimeReady.then(function() {
		try {
			d2wStart(message);
		} catch (error) {
			postMessage({ type: 'error', message: String((error && error.message) || error) });
		}
	});
};
