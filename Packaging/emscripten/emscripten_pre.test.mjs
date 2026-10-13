import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';
import vm from 'node:vm';

test('browser persistence mounts the directory returned by SDL3', async () => {
	const directories = new Set(['/']);
	const mounts = [];
	const FS = {
		stat(path) {
			if (!directories.has(path))
				throw new Error(`missing: ${path}`);
			return { mode: 0x4000 };
		},
		isDir(mode) {
			return mode === 0x4000;
		},
		mkdir(path) {
			directories.add(path);
		},
		mount(_filesystem, _options, path) {
			mounts.push(path);
		},
		unmount() {},
		syncfs(_populate, callback) {
			callback(null);
		},
		readdir() {
			return ['.', '..'];
		},
	};
	const context = vm.createContext({
		console,
		fetch: () => Promise.reject(new Error('not used')),
		FS,
		IDBFS: {},
		Module: { preRun: [], postRun: [] },
		Promise,
		setInterval() {},
		window: { addEventListener() {} },
	});
	const source = await readFile(new URL('./emscripten_pre.js', import.meta.url), 'utf8');
	vm.runInContext(source, context);

	const persistence = context.window.DvlBrowserPersistence;
	await persistence.restore();

	assert.equal(persistence.getPath(), '/libsdl/diasurgical');
	assert.equal(mounts.at(-1), persistence.getPath());
});
