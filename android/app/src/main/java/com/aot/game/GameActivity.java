package com.aot.game;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.text.TextUtils;
import android.view.Gravity;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import com.chaquo.python.PyObject;
import com.chaquo.python.Python;
import com.chaquo.python.android.AndroidPlatform;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class GameActivity extends NativeActivity {
    private static final int PICK_GAME_FOLDER = 4107;
    private final ExecutorService importer = Executors.newSingleThreadExecutor();
    private static final String[] RUNTIME_SOURCES = {
            "shims.c", "win_file.c", "win_audio.c", "win_gfx.c", "win_d3d10.c"
    };
    private TextView progressText;
    private AlertDialog progressDialog;
    private int copiedFiles;
    private long copiedBytes;

    private static final class GameInventory {
        int executables;
        int libraries;
        int validPeFiles;
        int x64Executables;
        String rootX64Executable;
        String firstX64Executable;
        final List<String> details = new ArrayList<>();
    }

    private String relativePath(File root, File entry) throws Exception {
        String rootPath = root.getCanonicalPath() + File.separator;
        String entryPath = entry.getCanonicalPath();
        if (!entryPath.startsWith(rootPath)) {
            throw new IllegalStateException("An executable escaped the selected game folder.");
        }
        return entryPath.substring(rootPath.length());
    }

    private boolean isValidPeArchitecture(String architecture) {
        return !"not a PE file".equals(architecture)
                && !"invalid PE header".equals(architecture)
                && !"invalid PE signature".equals(architecture)
                && !"unreadable".equals(architecture);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        boolean bundled = false;
        try (InputStream in = getAssets().open("game.exe")) { bundled = true; } catch (Exception ignored) { }
        bundledMode = bundled;
        if (state == null && !bundled) {
            showFolderPrompt();
        }
    }

    // ---- bundled-game mode: run in place from a user-selected folder (no copying) ----
    private boolean bundledMode, gateOpen;

    @Override
    protected void onResume() {
        super.onResume();
        if (bundledMode) ensureGameAccess();
    }

    private File gameDirFile() { File d = getExternalFilesDir(null); return d == null ? null : new File(d, "game_dir.txt"); }

    private void ensureGameAccess() {
        if (gateOpen) return;
        File f = gameDirFile();
        if (f != null && f.isFile()) return;   // folder already chosen; the native thread is running the game
        boolean ok;
        if (android.os.Build.VERSION.SDK_INT >= 30) ok = android.os.Environment.isExternalStorageManager();
        else ok = checkSelfPermission(android.Manifest.permission.READ_EXTERNAL_STORAGE) == android.content.pm.PackageManager.PERMISSION_GRANTED;
        gateOpen = true;
        if (!ok) {
            new AlertDialog.Builder(this).setTitle("Storage access needed").setCancelable(false)
                    .setMessage("The game runs directly from your game folder, so it needs permission to read your files. Grant \"All files access\" on the next screen, then come back.")
                    .setNegativeButton("Exit", (d, w) -> finish())
                    .setPositiveButton("Grant access", (d, w) -> {
                        gateOpen = false;
                        if (android.os.Build.VERSION.SDK_INT >= 30) {
                            startActivity(new Intent(android.provider.Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
                        } else {
                            requestPermissions(new String[]{android.Manifest.permission.READ_EXTERNAL_STORAGE, android.Manifest.permission.WRITE_EXTERNAL_STORAGE}, 7);
                        }
                    }).show();
            return;
        }
        new AlertDialog.Builder(this).setTitle("Choose game folder").setCancelable(false)
                .setMessage("Select the folder that contains the game (the one with the .exe and its data files). Nothing is copied.")
                .setNegativeButton("Exit", (d, w) -> finish())
                .setPositiveButton("Choose folder", (d, w) -> { gateOpen = false; openFolderPicker(); }).show();
    }

    private String treeToPath(Uri tree) {
        String id = DocumentsContract.getTreeDocumentId(tree);   // "primary:Games/AC1" or "1234-ABCD:Games/AC1"
        int c = id.indexOf(':'); String vol = c < 0 ? id : id.substring(0, c), rel = c < 0 ? "" : id.substring(c + 1);
        String base = vol.equals("primary") ? android.os.Environment.getExternalStorageDirectory().getAbsolutePath() : "/storage/" + vol;
        return rel.isEmpty() ? base : base + "/" + rel;
    }

    private void handleBundledPick(Intent data) {
        File dir = new File(treeToPath(data.getData()));
        String[] names = dir.list();
        boolean hasExe = false;
        if (names != null) for (String n : names) if (n.toLowerCase().endsWith(".exe")) hasExe = true;
        if (names == null || !hasExe) {
            gateOpen = false;
            new AlertDialog.Builder(this).setTitle("Cannot use that folder").setCancelable(false)
                    .setMessage(names == null ? "The folder " + dir + " is not readable. Make sure All files access is granted, then pick it again." : "No .exe was found in " + dir + ". Pick the folder that contains the game executable.")
                    .setPositiveButton("Choose again", (d, w) -> ensureGameAccess()).show();
            return;
        }
        try (FileOutputStream o = new FileOutputStream(gameDirFile())) { o.write((dir.getAbsolutePath() + "\n").getBytes("UTF-8")); }
        catch (Exception e) { showError("Could not save the game folder: " + e.getMessage()); return; }
        android.widget.Toast.makeText(this, "Starting game from " + dir + " (log: aot_log.txt in that folder)", android.widget.Toast.LENGTH_LONG).show();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_GAME_FOLDER) return;
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            if (bundledMode) { gateOpen = false; ensureGameAccess(); } else showFolderPrompt();
            return;
        }
        if (bundledMode) { handleBundledPick(data); return; }

        Uri tree = data.getData();
        int flags = data.getFlags()
                & (Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        try {
            getContentResolver().takePersistableUriPermission(tree, flags);
        } catch (SecurityException e) {
            showError("Android did not grant persistent access to that folder: " + e.getMessage());
            return;
        }
        beginImport(tree);
    }

    private void showFolderPrompt() {
        new AlertDialog.Builder(this)
                .setTitle("Choose game folder")
                .setMessage("Allow access to the folder containing the game executable and its data. The selected files will be copied into this app's private game folder.")
                .setCancelable(false)
                .setNegativeButton("Exit", (dialog, which) -> finish())
                .setPositiveButton("Choose folder", (dialog, which) -> openFolderPicker())
                .show();
    }

    private void openFolderPicker() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION
                | Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        startActivityForResult(intent, PICK_GAME_FOLDER);
    }

    private void beginImport(Uri tree) {
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setPadding(48, 32, 48, 24);
        ProgressBar spinner = new ProgressBar(this);
        spinner.setIndeterminate(true);
        progressText = new TextView(this);
        progressText.setGravity(Gravity.CENTER);
        progressText.setPadding(0, 24, 0, 0);
        progressText.setText("Copying game files...");
        layout.addView(spinner);
        layout.addView(progressText);
        progressDialog = new AlertDialog.Builder(this)
                .setTitle("Importing game folder")
                .setView(layout)
                .setCancelable(false)
                .create();
        progressDialog.show();

        importer.execute(() -> {
            try {
                copiedFiles = 0;
                copiedBytes = 0;
                File appData = getExternalFilesDir(null);
                if (appData == null) throw new IllegalStateException("App-specific storage is unavailable.");
                File staging = new File(appData, "game-importing");
                File destination = new File(appData, "game");
                File previous = new File(appData, "game-previous");
                deleteTree(staging);
                deleteTree(previous);
                if (!staging.mkdirs() && !staging.isDirectory()) {
                    throw new IllegalStateException("Could not create the app's game import folder.");
                }

                String rootId = DocumentsContract.getTreeDocumentId(tree);
                Uri root = DocumentsContract.buildDocumentUriUsingTree(tree, rootId);
                copyDirectory(tree, root, staging);

                if (destination.exists() && !destination.renameTo(previous)) {
                    throw new IllegalStateException("Could not replace the previous imported game folder.");
                }
                if (!staging.renameTo(destination)) {
                    if (previous.exists()) previous.renameTo(destination);
                    throw new IllegalStateException("Could not finish installing the selected game folder.");
                }
                deleteTree(previous);

                GameInventory inventory = inspectPeFiles(destination);
                String executable = inventory.rootX64Executable != null
                        ? inventory.rootX64Executable : inventory.firstX64Executable;
                if (executable == null) {
                    String summary = formatInventory(inventory);
                    runOnUiThread(() -> showImportResult(destination, inventory, summary, null));
                    return;
                }

                runOnUiThread(() -> {
                    if (progressText != null) {
                        progressText.setText("Translating the Windows executable on device...");
                    }
                });
                File runtimeSources = installRuntimeSources(appData);
                File translationRoot = new File(appData, "translated");
                File output = new File(translationRoot, safeOutputName(executable));
                deleteTree(output);
                if (!output.mkdirs() && !output.isDirectory()) {
                    throw new IllegalStateException("Could not create the translation output folder.");
                }
                if (!Python.isStarted()) Python.start(new AndroidPlatform(getApplicationContext()));
                String executablePath = new File(destination, executable).getCanonicalPath();
                PyObject result = Python.getInstance().getModule("aot_android").callAttr(
                        "translate",
                        executablePath,
                        output.getAbsolutePath(),
                        runtimeSources.getAbsolutePath(),
                        getApplicationInfo().nativeLibraryDir);
                String summary = formatInventory(inventory);
                runOnUiThread(() -> showImportResult(destination, inventory, summary, result.toString()));
            } catch (Exception e) {
                runOnUiThread(() -> {
                    dismissProgress();
                    showError("Game import or on-device translation failed: " + e.getMessage());
                });
            }
        });
    }

    private File installRuntimeSources(File appData) throws Exception {
        File destination = new File(appData, "runtime_sources");
        if (!destination.mkdirs() && !destination.isDirectory()) {
            throw new IllegalStateException("Could not prepare the runtime API manifest sources.");
        }
        for (String name : RUNTIME_SOURCES) {
            File output = new File(destination, name);
            try (InputStream input = getAssets().open("runtime_sources/" + name);
                 FileOutputStream stream = new FileOutputStream(output)) {
                byte[] buffer = new byte[16 * 1024];
                int count;
                while ((count = input.read(buffer)) != -1) stream.write(buffer, 0, count);
            }
        }
        return destination;
    }

    private String safeOutputName(String executable) {
        String base = new File(executable).getName().replaceAll("[^A-Za-z0-9._-]", "_");
        return base + ".translated";
    }

    private void copyDirectory(Uri tree, Uri directoryUri, File destination) throws Exception {
        String documentId = DocumentsContract.getDocumentId(directoryUri);
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, documentId);
        String[] columns = {
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE
        };
        try (Cursor cursor = getContentResolver().query(children, columns, null, null, null)) {
            if (cursor == null) throw new IllegalStateException("The selected folder could not be read.");
            int idColumn = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DOCUMENT_ID);
            int nameColumn = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_DISPLAY_NAME);
            int mimeColumn = cursor.getColumnIndexOrThrow(DocumentsContract.Document.COLUMN_MIME_TYPE);
            while (cursor.moveToNext()) {
                String name = cursor.getString(nameColumn);
                if (TextUtils.isEmpty(name) || name.equals(".") || name.equals("..")
                        || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0) {
                    throw new IllegalStateException("The provider returned an unsafe file name.");
                }
                File output = new File(destination, name).getCanonicalFile();
                String parent = destination.getCanonicalPath() + File.separator;
                if (!output.getPath().startsWith(parent)) {
                    throw new IllegalStateException("A selected file name escapes the import folder.");
                }
                String childId = cursor.getString(idColumn);
                Uri childUri = DocumentsContract.buildDocumentUriUsingTree(tree, childId);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(cursor.getString(mimeColumn))) {
                    if (!output.mkdirs() && !output.isDirectory()) {
                        throw new IllegalStateException("Could not create " + name);
                    }
                    copyDirectory(tree, childUri, output);
                } else {
                    copyFile(childUri, output);
                }
            }
        }
    }

    private void copyFile(Uri source, File destination) throws Exception {
        long length = 0;
        try (InputStream input = getContentResolver().openInputStream(source);
             FileOutputStream output = new FileOutputStream(destination)) {
            if (input == null) throw new IllegalStateException("A selected file could not be opened.");
            byte[] buffer = new byte[64 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) {
                output.write(buffer, 0, count);
                length += count;
            }
        }
        copiedFiles++;
        copiedBytes += length;
        if (copiedFiles % 20 == 0) {
            final int count = copiedFiles;
            final long bytes = copiedBytes;
            runOnUiThread(() -> {
                if (progressText != null) {
                    progressText.setText("Copied " + count + " files (" + formatBytes(bytes) + ")");
                }
            });
        }
    }

    private GameInventory inspectPeFiles(File root) throws Exception {
        GameInventory result = new GameInventory();
        inspectDirectory(root, root, result);
        return result;
    }

    private void inspectDirectory(File root, File directory, GameInventory result) throws Exception {
        File[] entries = directory.listFiles();
        if (entries == null) throw new IllegalStateException("Could not inspect " + directory.getName());
        for (File entry : entries) {
            if (entry.isDirectory()) {
                inspectDirectory(root, entry, result);
            } else {
                String name = entry.getName().toLowerCase(java.util.Locale.ROOT);
                boolean executable = name.endsWith(".exe");
                boolean library = name.endsWith(".dll");
                if (!executable && !library) continue;
                if (executable) result.executables++;
                else result.libraries++;
                String architecture = peArchitecture(entry);
                if (isValidPeArchitecture(architecture)) result.validPeFiles++;
                if (executable && ("x86-64".equals(architecture) || "x86".equals(architecture))) {
                    result.x64Executables++;
                    String relative = relativePath(root, entry);
                    if (result.firstX64Executable == null) result.firstX64Executable = relative;
                    if (entry.getParentFile().equals(root)) result.rootX64Executable = relative;
                }
                if (result.details.size() < 12) {
                    String relative = root.toURI().relativize(entry.toURI()).getPath();
                    result.details.add(relative + " - " + architecture);
                }
            }
        }
    }

    private String peArchitecture(File file) {
        try (RandomAccessFile input = new RandomAccessFile(file, "r")) {
            if (input.length() < 64 || input.readUnsignedByte() != 'M' || input.readUnsignedByte() != 'Z') {
                return "not a PE file";
            }
            input.seek(0x3c);
            long peOffset = readU32(input);
            if (peOffset > input.length() - 6) return "invalid PE header";
            input.seek(peOffset);
            if (input.readUnsignedByte() != 'P' || input.readUnsignedByte() != 'E'
                    || input.readUnsignedByte() != 0 || input.readUnsignedByte() != 0) {
                return "invalid PE signature";
            }
            int machine = input.readUnsignedByte() | (input.readUnsignedByte() << 8);
            switch (machine) {
                case 0x8664: return "x86-64";
                case 0x014c: return "x86";
                case 0xaa64: return "ARM64";
                case 0x01c4: return "ARM";
                default: return String.format(java.util.Locale.ROOT, "PE machine 0x%04x", machine);
            }
        } catch (Exception e) {
            return "unreadable";
        }
    }

    private long readU32(RandomAccessFile input) throws Exception {
        return (input.readUnsignedByte())
                | ((long) input.readUnsignedByte() << 8)
                | ((long) input.readUnsignedByte() << 16)
                | ((long) input.readUnsignedByte() << 24);
    }

    private String formatInventory(GameInventory inventory) {
        StringBuilder text = new StringBuilder();
        text.append("Found ").append(inventory.executables).append(" EXE(s), ")
                .append(inventory.libraries).append(" DLL(s), and ")
                .append(inventory.validPeFiles).append(" valid PE file(s). ")
                .append(inventory.x64Executables).append(" EXE(s) are x86 / x86-64 (supported).\n\n");
        if (inventory.details.isEmpty()) {
            text.append("No EXE or DLL files were found.");
        } else {
            for (String item : inventory.details) text.append(item).append('\n');
            if (inventory.executables + inventory.libraries > inventory.details.size()) {
                text.append("Additional files omitted from this summary.\n");
            }
        }
        return text.toString();
    }

    private void showImportResult(File gameFolder, GameInventory inventory, String summary,
                                  String translationJson) {
        dismissProgress();
        String resultText;
        if (translationJson == null) {
            resultText = summary + "\n\nNo x86 or x86-64 Windows executable was found.";
        } else {
            try {
                org.json.JSONObject result = new org.json.JSONObject(translationJson);
                org.json.JSONArray unsupported = result.getJSONArray("unsupported_instructions");
                org.json.JSONArray missingDlls = result.getJSONArray("missing_dlls");
                org.json.JSONArray missingFunctions = result.getJSONArray("missing_functions");
                int unsupportedInstructions = 0;
                for (int i = 0; i < unsupported.length(); i++) {
                    unsupportedInstructions += unsupported.getJSONObject(i).getInt("count");
                }
                resultText = summary
                        + "\nTranslated " + result.getInt("instruction_count") + " instructions in "
                        + result.getInt("block_count") + " blocks (" + result.getInt("region_count")
                        + " regions).\nGenerated C and reports:\n" + result.getString("output_directory")
                        + "\n\nUnsupported instructions: " + unsupportedInstructions
                        + "\nMissing DLLs: " + missingDlls.length()
                        + "\nMissing imported functions: " + missingFunctions.length()
                        + "\nDependency report: " + result.getString("dependency_report")
                        + "\n\nTranslation output is not compiled or bootable yet; this APK does not "
                        + "have an on-device native C compiler or dynamic runtime loader.";
            } catch (org.json.JSONException e) {
                showError("The on-device translator returned an invalid result: " + e.getMessage());
                return;
            }
        }
        new AlertDialog.Builder(this)
                .setTitle(translationJson == null ? "Game files imported" : "Translation complete")
                .setMessage(resultText + "\n\nGame folder: " + gameFolder.getAbsolutePath())
                .setCancelable(false)
                .setPositiveButton("Choose another folder", (dialog, which) -> openFolderPicker())
                .setNegativeButton("Close", (dialog, which) -> finish())
                .show();
    }

    private void showError(String message) {
        new AlertDialog.Builder(this)
                .setTitle("Game folder error")
                .setMessage(message)
                .setPositiveButton("Try again", (dialog, which) -> showFolderPrompt())
                .setNegativeButton("Exit", (dialog, which) -> finish())
                .setCancelable(false)
                .show();
    }

    private void dismissProgress() {
        if (progressDialog != null && progressDialog.isShowing()) progressDialog.dismiss();
        progressDialog = null;
        progressText = null;
    }

    private void deleteTree(File path) throws Exception {
        if (!path.exists()) return;
        if (path.isDirectory()) {
            File[] children = path.listFiles();
            if (children == null) throw new IllegalStateException("Could not clean app import staging data.");
            for (File child : children) deleteTree(child);
        }
        if (!path.delete()) throw new IllegalStateException("Could not clean app import staging data.");
    }

    private String formatBytes(long bytes) {
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024 * 1024) return (bytes / 1024) + " KB";
        return (bytes / (1024 * 1024)) + " MB";
    }
}
