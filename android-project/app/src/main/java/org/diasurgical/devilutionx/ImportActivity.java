package org.diasurgical.devilutionx;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ComponentName;
import android.content.ContentResolver;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.support.annotation.Nullable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.StatFs;
import android.support.annotation.RequiresApi;
import android.support.v4.provider.DocumentFile;
import android.text.SpannableString;
import android.text.Spanned;
import android.text.method.LinkMovementMethod;
import android.text.style.URLSpan;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.BufferedInputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FilterInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public class ImportActivity extends Activity {

	private static final int IMPORT_REQUEST_CODE = 0xD1AB70;
	private static final String TV_STUB_PACKAGE = "com.android.tv.frameworkpackagestubs";
	private static final String RECOMMENDED_FILE_BROWSER = "AnExplorer";
	private static final String RECOMMENDED_FILE_BROWSER_PACKAGE = "dev.dworks.apps.anexplorer";
	/** Folder (in the data folder) where the game looks for HD art by default. */
	private static final String HD_FOLDER = "hd";
	private boolean userConfirmed = false;
	private boolean importing = false;

	@RequiresApi(api = Build.VERSION_CODES.KITKAT)
	@Override
	protected void onCreate(Bundle savedInstanceState) {
		super.onCreate(savedInstanceState);

		if (findFileBrowser()) {
			runFileBrowser();
		} else {
			recommendFileBrowser();
		}
	}

	@Override
	protected void onActivityResult(int requestCode, int resultCode, @Nullable Intent data) {
		if (requestCode != IMPORT_REQUEST_CODE)
			return;

		if (resultCode == Activity.RESULT_OK && data != null) {
			ArrayList<Uri> uriList = getItemUris(data.getClipData());

			Uri dataUri = data.getData();
			if (dataUri != null)
				uriList.add(dataUri);

			ArrayList<String> fileNames = getFileNames(uriList);
			ArrayList<String> overwrittenFiles = getOverwrittenFiles(fileNames);
			if (overwrittenFiles.isEmpty()) {
				importFiles(uriList);
				return;
			}

			String overwrittenFileList = String.join("\n", overwrittenFiles);
			AlertDialog.Builder builder = new AlertDialog.Builder(this);
			builder.setMessage(getString(R.string.overwrite_query, overwrittenFileList));
			builder.setPositiveButton(R.string.continue_button, (dialog, which) -> { importFiles(uriList); });
			builder.setNegativeButton(R.string.cancel_button, null);
			builder.setOnDismissListener(dialog -> { if (!importing) finish(); });

			AlertDialog dialog = builder.create();
			dialog.show();
		} else {
			super.onBackPressed();
		}
	}

	private boolean findFileBrowser() {
		Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
		intent.addCategory(Intent.CATEGORY_OPENABLE);
		intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
		intent.setType("*/*");

		PackageManager packageManager = getPackageManager();
		ComponentName defaultPackage = intent.resolveActivity(packageManager);
		return defaultPackage != null && !TV_STUB_PACKAGE.equals(defaultPackage.getPackageName());
	}

	private void recommendFileBrowser()
	{
		String noFileBrowserText = getString(R.string.no_file_browser);
		SpannableString message;

		boolean isTelevision = getPackageManager().hasSystemFeature(PackageManager.FEATURE_LEANBACK);
		if (isTelevision) {
			String recommendation = getString(R.string.recommended_file_browser, RECOMMENDED_FILE_BROWSER);
			String messageText = noFileBrowserText + " " + recommendation;
			message = new SpannableString(messageText);

			int index = messageText.indexOf(RECOMMENDED_FILE_BROWSER);
			if (index >= 0) {
				String url = "market://details?id=" + RECOMMENDED_FILE_BROWSER_PACKAGE;
				URLSpan urlSpan = new URLSpan(url);
				message.setSpan(urlSpan, index, index + RECOMMENDED_FILE_BROWSER.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
			}
		} else {
			message = new SpannableString(noFileBrowserText);
		}

		AlertDialog.Builder builder = new AlertDialog.Builder(this);
		builder.setMessage(message);
		builder.setPositiveButton("OK", null);
		builder.setOnDismissListener(dialog -> {
			if (findFileBrowser()) {
				runFileBrowser();
			} else if (!isFinishing()) {
				finish();
			}
		});

		AlertDialog dialog = builder.create();
		dialog.show();

		View view = dialog.findViewById(android.R.id.message);
		if (view instanceof TextView) {
			TextView textView = (TextView)view;
			textView.setMovementMethod(LinkMovementMethod.getInstance());
		}
	}

	private void runFileBrowser() {
		ExternalFilesManager fileManager = new ExternalFilesManager(this);
		String externalFilesDir =  fileManager.getExternalFilesDirectory();

		AlertDialog.Builder builder = new AlertDialog.Builder(this);
		String info = getString(R.string.import_data_info, externalFilesDir);
		if (BuildConfig.WEBGPU)
			info += "\n\n" + getString(R.string.import_hd_info);
		builder.setMessage(info);
		builder.setPositiveButton(R.string.ok_button, (dialog, which) -> {
			userConfirmed = true;
			Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
			intent.addCategory(Intent.CATEGORY_OPENABLE);
			intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
			intent.setType("*/*");
			startActivityForResult(intent, IMPORT_REQUEST_CODE);
		});
		builder.setOnDismissListener(dialog -> {
			if (!userConfirmed && !isFinishing()) finish();
		});

		AlertDialog dialog = builder.create();
		dialog.show();
	}

	private ArrayList<Uri> getItemUris(ClipData clipData) {
		ArrayList<Uri> uriList = new ArrayList<>();
		if (clipData == null)
			return uriList;

		for (int i = 0; i < clipData.getItemCount(); i++) {
			ClipData.Item item = clipData.getItemAt(i);
			if (item == null)
				continue;

			Uri itemUri = item.getUri();
			if (itemUri == null)
				continue;

			uriList.add(itemUri);
		}

		return uriList;
	}

	private ArrayList<String> getFileNames(ArrayList<Uri> uriList) {
		ArrayList<String> fileNames = new ArrayList<>();
		for (Uri uri : uriList) {
			DocumentFile file = DocumentFile.fromSingleUri(getApplicationContext(), uri);
			if (file == null)
				continue;

			String fileName = file.getName();
			fileNames.add(fileName);
		}
		return fileNames;
	}

	private ArrayList<String> getOverwrittenFiles(ArrayList<String> fileNames) {
		ArrayList<String> overwrittenFiles = new ArrayList<>();
		ExternalFilesManager fileManager = new ExternalFilesManager(this);
		boolean hdListed = false;
		for (String fileName : fileNames) {
			if (isHdPack(fileName)) {
				if (!hdListed && fileManager.hasFile(HD_FOLDER)) {
					overwrittenFiles.add(HD_FOLDER + "/");
					hdListed = true;
				}
			} else if (fileManager.hasFile(fileName)) {
				overwrittenFiles.add(fileName);
			}
		}
		return overwrittenFiles;
	}

	/** A .zip is a DamnationX HD pack: it is unpacked into the `hd` folder (where the game looks by default) instead of copied. */
	private static boolean isHdPack(String fileName) {
		return fileName != null && fileName.toLowerCase(Locale.ROOT).endsWith(".zip");
	}

	/** Copies (or, for HD packs, unpacks) the files on a worker thread behind a progress dialog, then finishes. */
	private void importFiles(ArrayList<Uri> uriList) {
		getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

		int padding = (int) (16 * getResources().getDisplayMetrics().density);
		LinearLayout layout = new LinearLayout(this);
		layout.setOrientation(LinearLayout.VERTICAL);
		layout.setPadding(padding, padding, padding, padding);
		TextView label = new TextView(this);
		ProgressBar bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
		bar.setMax(1000);
		layout.addView(label);
		layout.addView(bar);

		AlertDialog progressDialog = new AlertDialog.Builder(this)
				.setTitle(R.string.import_progress_title)
				.setView(layout)
				.setCancelable(false)
				.create();
		progressDialog.show();
		importing = true;

		Handler ui = new Handler(Looper.getMainLooper());
		new Thread(() -> {
			ArrayList<String> messages = new ArrayList<>();
			for (Uri uri : uriList) {
				try {
					String message = importFile(uri, (text, fraction) -> ui.post(() -> {
						label.setText(text);
						bar.setProgress((int) (fraction * 1000));
					}));
					if (message != null)
						messages.add(message);
				} catch (IOException | RuntimeException exception) {
					Log.e("importFile", "Import failed", exception);
					String detail = exception.getMessage() != null ? exception.getMessage() : exception.toString();
					messages.add(getString(R.string.import_failed, detail));
				}
			}
			ui.post(() -> {
				importing = false;
				getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
				if (!isFinishing() && !isDestroyed())
					progressDialog.dismiss();
				if (messages.isEmpty() || isFinishing() || isDestroyed()) {
					finish();
					return;
				}
				new AlertDialog.Builder(this)
						.setMessage(String.join("\n\n", messages))
						.setPositiveButton(R.string.ok_button, null)
						.setOnDismissListener(dialog -> finish())
						.show();
			});
		}, "import").start();
	}

	private interface Progress {
		void update(String text, double fraction);
	}

	/** @return a message for the user, or null */
	@Nullable
	private String importFile(Uri fileUri, Progress progress) throws IOException {
		if (fileUri == null)
			return null;

		DocumentFile file = DocumentFile.fromSingleUri(getApplicationContext(), fileUri);
		if (file == null)
			return null;

		String fileName = file.getName();
		long size = file.length();
		ExternalFilesManager fileManager = new ExternalFilesManager(this);
		File externalDir = new File(fileManager.getExternalFilesDirectory());

		// HD packs barely compress (WebP/PNG), so the unpacked size is about the zip size.
		long freeBytes = new StatFs(externalDir.getAbsolutePath()).getAvailableBytes();
		long neededBytes = size + 64L * 1024 * 1024;
		if (size > 0 && freeBytes < neededBytes)
			return getString(R.string.import_no_space, fileName, (int) (neededBytes >> 20), (int) (freeBytes >> 20));

		ContentResolver contentResolver = getContentResolver();
		try (InputStream raw = contentResolver.openInputStream(fileUri)) {
			if (raw == null)
				throw new IOException("Cannot open " + fileName);
			String text = getString(isHdPack(fileName) ? R.string.import_progress_hd : R.string.import_progress_file, fileName);
			CountingInputStream inputStream = new CountingInputStream(raw, read -> progress.update(text, size > 0 ? (double) read / size : 0));
			if (isHdPack(fileName))
				return importHdPack(inputStream, fileName, externalDir);

			File externalFile = fileManager.getFile(fileName);
			File temp = new File(externalFile.getPath() + "~");
			try (OutputStream outputStream = new FileOutputStream(temp)) {
				copy(inputStream, outputStream);
			} catch (IOException exception) {
				//noinspection ResultOfMethodCallIgnored
				temp.delete();
				throw exception;
			}
			//noinspection ResultOfMethodCallIgnored
			externalFile.delete();
			if (!temp.renameTo(externalFile))
				throw new IOException("Cannot rename " + temp + " to " + externalFile);
			return null;
		}
	}

	/**
	 * Unpacks the zip into a staging folder, finds the folder holding sprites.txt (the release zips have it under
	 * `hd/`) and swaps it in as `hd`, replacing the previous pack as a whole (see the pack's README).
	 */
	private String importHdPack(InputStream inputStream, String fileName, File externalDir) throws IOException {
		File staging = new File(externalDir, HD_FOLDER + ".importing");
		deleteRecursively(staging);
		if (!staging.mkdirs())
			throw new IOException("Cannot create " + staging);
		String stagingRoot = staging.getCanonicalPath() + File.separator;
		int fileCount = 0;
		try {
			try (ZipInputStream zip = new ZipInputStream(new BufferedInputStream(inputStream, 1 << 16))) {
				ZipEntry entry;
				while ((entry = zip.getNextEntry()) != null) {
					File target = new File(staging, entry.getName());
					if (!target.getCanonicalPath().startsWith(stagingRoot))
						throw new IOException("Unsafe path in zip: " + entry.getName());
					if (entry.isDirectory()) {
						//noinspection ResultOfMethodCallIgnored
						target.mkdirs();
						continue;
					}
					File parent = target.getParentFile();
					if (parent != null && !parent.isDirectory() && !parent.mkdirs())
						throw new IOException("Cannot create " + parent);
					try (OutputStream out = new FileOutputStream(target)) {
						copy(zip, out);
					}
					fileCount++;
				}
			}

			File packRoot = findPackRoot(staging, 3);
			if (packRoot == null)
				return getString(R.string.import_hd_not_pack, fileName);

			File hd = new File(externalDir, HD_FOLDER);
			File old = new File(externalDir, HD_FOLDER + ".old");
			deleteRecursively(old);
			if (hd.exists() && !hd.renameTo(old))
				throw new IOException("Cannot move the old " + hd);
			if (!packRoot.renameTo(hd)) {
				//noinspection ResultOfMethodCallIgnored
				old.renameTo(hd);
				throw new IOException("Cannot move " + packRoot + " to " + hd);
			}
			deleteRecursively(old);
			return getString(R.string.import_hd_done, fileCount);
		} finally {
			deleteRecursively(staging);
		}
	}

	/** The shallowest folder (breadth first, up to maxDepth below dir) that contains sprites.txt. */
	@Nullable
	private static File findPackRoot(File dir, int maxDepth) {
		ArrayList<File> level = new ArrayList<>();
		level.add(dir);
		for (int depth = 0; depth <= maxDepth && !level.isEmpty(); depth++) {
			ArrayList<File> next = new ArrayList<>();
			for (File folder : level) {
				if (new File(folder, "sprites.txt").isFile())
					return folder;
				File[] children = folder.listFiles(File::isDirectory);
				if (children != null)
					Collections.addAll(next, children);
			}
			level = next;
		}
		return null;
	}

	private static void deleteRecursively(File file) {
		File[] children = file.listFiles();
		if (children != null) {
			for (File child : children)
				deleteRecursively(child);
		}
		//noinspection ResultOfMethodCallIgnored
		file.delete();
	}

	private static void copy(InputStream in, OutputStream out) throws IOException {
		byte[] buf = new byte[1 << 16];
		int len;
		while ((len = in.read(buf)) > 0)
			out.write(buf, 0, len);
	}

	/** Reports bytes read from the source (at most every 256 KiB) so progress follows the compressed zip. */
	private static class CountingInputStream extends FilterInputStream {
		interface Listener {
			void onRead(long total);
		}

		private final Listener listener;
		private long total;
		private long reported;

		CountingInputStream(InputStream in, Listener listener) {
			super(in);
			this.listener = listener;
			listener.onRead(0);
		}

		private void count(long n) {
			if (n <= 0)
				return;
			total += n;
			if (total - reported >= (256 << 10)) {
				reported = total;
				listener.onRead(total);
			}
		}

		@Override
		public int read() throws IOException {
			int b = super.read();
			if (b >= 0)
				count(1);
			return b;
		}

		@Override
		public int read(byte[] b, int off, int len) throws IOException {
			int n = super.read(b, off, len);
			count(n);
			return n;
		}

		@Override
		public long skip(long n) throws IOException {
			long skipped = super.skip(n);
			count(skipped);
			return skipped;
		}
	}
}
