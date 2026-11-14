package com.aot.game;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.text.TextUtils;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.widget.PopupWindow;
import android.view.ViewGroup;
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
    static { try { System.loadLibrary("aotgame"); } catch (Throwable ignored) { } }   // JNI input hooks live in the same library NativeActivity loads natively
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


    private static native void nativeKey0(int vk, int down);
    private static native void nativeMouse0(int kind, float x, float y);
    private static native void nativeView0(int w, int h);
    private static boolean nativesOk = true;
    private static void nativeKey(int vk, int down) { if (nativesOk) try { nativeKey0(vk, down); } catch (Throwable t) { nativesOk = false; android.util.Log.w("aotgame", "input natives unavailable: " + t); } }
    private static void nativeMouse(int kind, float x, float y) { if (nativesOk) try { nativeMouse0(kind, x, y); } catch (Throwable t) { nativesOk = false; android.util.Log.w("aotgame", "input natives unavailable: " + t); } }
    private static void nativeView(int w, int h) { if (nativesOk) try { nativeView0(w, h); } catch (Throwable t) { nativesOk = false; android.util.Log.w("aotgame", "input natives unavailable: " + t); } }

    /* Physical gamepad. Its key/motion events are delivered to this Activity, not to the native input
       queue, so a real pad used to be invisible to the guest. Forward buttons, both sticks and both
       triggers to the guest's XInput device (buttons as the XINPUT_GAMEPAD bitmask) and let the game
       drive the camera from the right stick itself, the way it does with an Xbox 360 pad on Windows. */
    private static native void nativePad0(int buttons, int lt, int rt, int lx, int ly, int rx, int ry);
    private static native void nativePadPresent0(int on);
    private int padButtons, padLx, padLy, padRx, padRy, padLt, padRt;
    private int padPresent = -1;
    private static int padAxis(android.view.MotionEvent e, int a) {
        float v = e.getAxisValue(a); if (v > 1f) v = 1f; if (v < -1f) v = -1f; return (int) (v * 32767f); }
    private void sendPad() {
        if (!nativesOk) return;
        try { nativePad0(padButtons, padLt, padRt, padLx, padLy, padRx, padRy); }
        catch (Throwable t) { nativesOk = false; android.util.Log.w("aotgame", "input natives unavailable: " + t); } }
    private boolean padKey(int code, boolean down) {
        int bit;
        switch (code) {
            case android.view.KeyEvent.KEYCODE_BUTTON_A: bit = 0x1000; break;
            case android.view.KeyEvent.KEYCODE_BUTTON_B: bit = 0x2000; break;
            case android.view.KeyEvent.KEYCODE_BUTTON_X: bit = 0x4000; break;
            case android.view.KeyEvent.KEYCODE_BUTTON_Y: bit = 0x8000; break;
            case android.view.KeyEvent.KEYCODE_BUTTON_L1: bit = 0x100; break;
            case android.view.KeyEvent.KEYCODE_BUTTON_R1: bit = 0x200; break;
            case android.view.KeyEvent.KEYCODE_BUTTON_THUMBL: bit = 0x40; break;
            case android.view.KeyEvent.KEYCODE_BUTTON_THUMBR: bit = 0x80; break;
            case android.view.KeyEvent.KEYCODE_BUTTON_START: bit = 0x10; break;
            case android.view.KeyEvent.KEYCODE_BUTTON_SELECT: bit = 0x20; break;
            case android.view.KeyEvent.KEYCODE_DPAD_UP: bit = 1; break;
            case android.view.KeyEvent.KEYCODE_DPAD_DOWN: bit = 2; break;
            case android.view.KeyEvent.KEYCODE_DPAD_LEFT: bit = 4; break;
            case android.view.KeyEvent.KEYCODE_DPAD_RIGHT: bit = 8; break;
            case android.view.KeyEvent.KEYCODE_BUTTON_L2: padLt = down ? 255 : 0; sendPad(); return true;
            case android.view.KeyEvent.KEYCODE_BUTTON_R2: padRt = down ? 255 : 0; sendPad(); return true;
            default: return false;
        }
        if (down) padButtons |= bit; else padButtons &= ~bit;
        sendPad(); return true;
    }
    @Override public boolean onKeyDown(int code, android.view.KeyEvent e) { return padKey(code, true) || super.onKeyDown(code, e); }
    @Override public boolean onKeyUp(int code, android.view.KeyEvent e) { return padKey(code, false) || super.onKeyUp(code, e); }
    @Override public boolean onGenericMotionEvent(android.view.MotionEvent e) {
        int src = e.getSource();
        if ((src & android.view.InputDevice.SOURCE_JOYSTICK) != 0 || (src & android.view.InputDevice.SOURCE_GAMEPAD) != 0) {
            padLx = padAxis(e, android.view.MotionEvent.AXIS_X); padLy = padAxis(e, android.view.MotionEvent.AXIS_Y);
            padRx = padAxis(e, android.view.MotionEvent.AXIS_Z); padRy = padAxis(e, android.view.MotionEvent.AXIS_RZ);
            padLt = (int) (e.getAxisValue(android.view.MotionEvent.AXIS_LTRIGGER) * 255f);
            padRt = (int) (e.getAxisValue(android.view.MotionEvent.AXIS_RTRIGGER) * 255f);
            sendPad(); return true;
        }
        return super.onGenericMotionEvent(e);
    }

    /** Text-only on-screen controller. Left stick = W/A/S/D; labelled round buttons = keys; "KB" opens a typing keyboard; any other touch = mouse
     *  (1st finger left click/drag, 2nd finger right click). Everything is drawn with plain text and circles/rectangles, nothing is loaded. */
    private static final class ControlsView extends View {
        private static final class Btn {
            final String label; final int vk; final int mk; final float cx, cy, hw, hh; final boolean sticky; int ptr = -1; boolean on;
            Btn(String l, int v, float x, float y, float w, float h, boolean s) { label = l; vk = v; mk = 0; cx = x; cy = y; hw = w; hh = h; sticky = s; }
            Btn(String l, int m, float x, float y, float w, float h) { label = l; mk = m; vk = 0; cx = x; cy = y; hw = w; hh = h; sticky = false; }
        }
        private final java.util.ArrayList<Btn> game = new java.util.ArrayList<>(), kb = new java.util.ArrayList<>();
        private final Btn kbToggle = new Btn("KB", -1, 0.93f, 0.08f, 0.05f, 0.07f, true);
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        private int stickPtr = -1, mousePtr = -1, rightPtr = -1; private float sx, sy, kx, ky; private final boolean[] dir = new boolean[4];   // W A S D
        private static final int[] DIRVK = { 0x57, 0x41, 0x53, 0x44 };

        ControlsView(Context c) {
            super(c);
            Object[][] g = { {"SPACE\njump/act", 0x20, 0.93f, 0.80f}, {"E\naction", 0x45, 0.85f, 0.64f}, {"SHIFT\nhigh prof.", 0x10, 0.76f, 0.84f}, {"CTRL\nlow prof.", 0x11, 0.68f, 0.66f},
                {"ENTER", 0x0D, 0.93f, 0.55f}, {"Q", 0x51, 0.58f, 0.84f}, {"TAB\nmap", 0x09, 0.50f, 0.68f}, {"ESC\nmenu", 0x1B, 0.06f, 0.08f}, {"F", 0x46, 0.85f, 0.43f}, {"R", 0x52, 0.76f, 0.50f} };
            for (Object[] o : g) game.add(new Btn((String) o[0], (Integer) o[1], (Float) o[2], (Float) o[3], 0.045f, 0.075f, false));
            /* Mouse buttons are their own buttons: a finger dragged across the surface only moves the pointer,
               so looking around cannot click. mk 1 = left, 3 = right; release sends mk + 1. */
            game.add(new Btn("LMB\nclick", 1, 0.93f, 0.30f, 0.045f, 0.075f));
            game.add(new Btn("RMB\naim", 3, 0.93f, 0.42f, 0.045f, 0.075f));
            String[] rows = { "1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
            for (int r = 0; r < rows.length; r++) for (int i = 0; i < rows[r].length(); i++) {
                char ch = rows[r].charAt(i); float x = 0.5f + (i - (rows[r].length() - 1) / 2f) * 0.075f;
                kb.add(new Btn(String.valueOf(ch), ch, x, 0.52f + r * 0.115f, 0.034f, 0.05f, false)); }
            kb.add(new Btn("BKSP", 0x08, 0.5f + 5.5f * 0.075f, 0.52f, 0.04f, 0.05f, false));
            kb.add(new Btn("ENTER", 0x0D, 0.5f + 5.2f * 0.075f, 0.52f + 2 * 0.115f, 0.05f, 0.05f, false));
            kb.add(new Btn("SHIFT", 0x10, 0.5f - 5.2f * 0.075f, 0.52f + 3 * 0.115f, 0.05f, 0.05f, true));
            kb.add(new Btn("SPACE", 0x20, 0.5f, 0.52f + 4 * 0.115f, 0.20f, 0.05f, false));
            kb.add(new Btn("ESC", 0x1B, 0.5f - 5.2f * 0.075f, 0.52f, 0.04f, 0.05f, false));
        }
        private float radius() { return Math.min(getWidth(), getHeight()) * 0.065f; }
        @Override protected void onSizeChanged(int w, int h, int ow, int oh) { nativeView(w, h); }
        private boolean inside(Btn b, float x, float y) { return Math.abs(x - b.cx * getWidth()) <= b.hw * getWidth() && Math.abs(y - b.cy * getHeight()) <= b.hh * getHeight(); }
        private java.util.List<Btn> active() { return kbToggle.on ? kb : game; }
        private Btn hit(float x, float y) {
            if (inside(kbToggle, x, y)) return kbToggle;
            for (Btn b : active()) if (inside(b, x, y)) return b; return null; }
        private void setDir(int i, boolean on) { if (dir[i] != on) { dir[i] = on; nativeKey(DIRVK[i], on ? 1 : 0); } }
        private void stick(float x, float y) {
            kx = x; ky = y; float dx = x - sx, dy = y - sy, dz = radius() * 0.6f;
            setDir(0, dy < -dz); setDir(2, dy > dz); setDir(1, dx < -dz); setDir(3, dx > dz); }
        private void releaseAll() {
            for (java.util.List<Btn> l : java.util.Arrays.asList(game, kb)) for (Btn b : l) { if (b.ptr >= 0 || b.on) { if (b.mk != 0) nativeMouse(b.mk + 1, 0, 0); else nativeKey(b.vk, 0); } b.ptr = -1; b.on = false; }
            for (int k = 0; k < 4; k++) setDir(k, false); stickPtr = -1; }
        @Override public boolean onTouchEvent(MotionEvent e) {
            try {
                int a = e.getActionMasked(), i = e.getActionIndex(), id = e.getPointerId(i);
                if (a == MotionEvent.ACTION_DOWN || a == MotionEvent.ACTION_POINTER_DOWN) {
                    float x = e.getX(i), y = e.getY(i); Btn b = hit(x, y);
                    if (b == kbToggle) { releaseAll(); kbToggle.on = !kbToggle.on; }
                    else if (b != null) { b.ptr = id; if (b.mk != 0) nativeMouse(b.mk, x, y); else if (b.sticky) { b.on = !b.on; nativeKey(b.vk, b.on ? 1 : 0); } else nativeKey(b.vk, 1); }
                    else if (!kbToggle.on && x < getWidth() * 0.30f && y > getHeight() * 0.35f && stickPtr < 0) { stickPtr = id; sx = x; sy = y; stick(x, y); }
                    else if (mousePtr < 0) { mousePtr = id; nativeMouse(0, x, y); }
                } else if (a == MotionEvent.ACTION_MOVE) {
                    for (int k = 0; k < e.getPointerCount(); k++) { int pid = e.getPointerId(k);
                        if (pid == stickPtr) stick(e.getX(k), e.getY(k)); else if (pid == mousePtr) nativeMouse(0, e.getX(k), e.getY(k)); }
                } else if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_POINTER_UP || a == MotionEvent.ACTION_CANCEL) {
                    float x = e.getX(i), y = e.getY(i); boolean cancel = a == MotionEvent.ACTION_CANCEL;
                    for (java.util.List<Btn> l : java.util.Arrays.asList(game, kb)) for (Btn b : l) if (b.ptr == id || cancel) { if (b.ptr >= 0) { if (b.mk != 0) nativeMouse(b.mk + 1, x, y); else if (!b.sticky) nativeKey(b.vk, 0); } b.ptr = -1; }
                    if (id == stickPtr || cancel) { stickPtr = -1; for (int k = 0; k < 4; k++) setDir(k, false); }
                    if (id == mousePtr || cancel) { mousePtr = -1; }
                }
                invalidate();
            } catch (Throwable t) { android.util.Log.w("aotgame", "controls: " + t); }
            return true;
        }
        private void drawBtn(Canvas c, Btn b, float w, float h, boolean pressed) {
            p.setStyle(Paint.Style.FILL); p.setColor(pressed ? 0x99FFFFFF : 0x40FFFFFF);
            c.drawRect((b.cx - b.hw) * w, (b.cy - b.hh) * h, (b.cx + b.hw) * w, (b.cy + b.hh) * h, p);
            p.setColor(0xFFFFFFFF); p.setTextAlign(Paint.Align.CENTER);
            String[] ln = b.label.split("\n"); float ts = Math.min(b.hh * h * (ln.length > 1 ? 0.55f : 0.9f), b.hw * w * 0.6f * (b.label.length() > 3 ? 0.8f : 1.2f)); p.setTextSize(ts);
            float y0 = b.cy * h - (ln.length - 1) * ts * 0.5f + ts * 0.35f; for (int k = 0; k < ln.length; k++) c.drawText(ln[k], b.cx * w, y0 + k * ts, p);
        }
        @Override protected void onDraw(Canvas c) {
            try {
                float r = radius(), w = getWidth(), h = getHeight();
                for (Btn b : active()) drawBtn(c, b, w, h, b.ptr >= 0 || b.on);
                drawBtn(c, kbToggle, w, h, kbToggle.on);
                if (!kbToggle.on) {
                    p.setStyle(Paint.Style.STROKE); p.setStrokeWidth(3); p.setColor(0x66FFFFFF);
                    float bx = stickPtr >= 0 ? sx : w * 0.14f, by = stickPtr >= 0 ? sy : h * 0.72f; c.drawCircle(bx, by, r * 1.8f, p);
                    p.setStyle(Paint.Style.FILL); p.setColor(0x44FFFFFF); c.drawCircle(stickPtr >= 0 ? kx : bx, stickPtr >= 0 ? ky : by, r * 0.8f, p);
                    p.setColor(0xFFFFFFFF); p.setTextAlign(Paint.Align.CENTER); p.setTextSize(r * 0.42f); c.drawText("STICK = W A S D", bx, by + r * 2.4f, p);
                    c.drawText("touch anywhere else = mouse (2nd finger = right click)", w * 0.5f, h * 0.97f, p);
                }
            } catch (Throwable t) { android.util.Log.w("aotgame", "controls draw: " + t); }
        }
    }

    private PopupWindow controlsWindow;
    private void jlog(String m) {
        android.util.Log.i("aotgame", m);
        try { File d = getExternalFilesDir(null); String gd = null;
            try (java.io.BufferedReader br = new java.io.BufferedReader(new java.io.FileReader(new File(d, "game_dir.txt")))) { gd = br.readLine(); } catch (Throwable ignored) { }
            File dir = gd != null && !gd.isEmpty() && new File(gd).isDirectory() ? new File(gd) : d;
            try (java.io.FileWriter w = new java.io.FileWriter(new File(dir, "aot_java_log.txt"), true)) { w.write(m + "\n"); } } catch (Throwable ignored) { }
    }
    /** NativeActivity draws straight into the window surface, so a normal child View never becomes visible. The overlay is a PopupWindow: its own surface, composited above the game. */
    private void addControls() {
        try {
            final View decor = getWindow().getDecorView();
            if (decor.getWindowToken() == null || decor.getWidth() == 0) { decor.postDelayed(this::addControls, 300); return; }
            if (controlsWindow != null && controlsWindow.isShowing()) return;
            ControlsView v = new ControlsView(this);
            controlsWindow = new PopupWindow(v, decor.getWidth(), decor.getHeight(), false);
            controlsWindow.setClippingEnabled(false); controlsWindow.setTouchable(true); controlsWindow.setBackgroundDrawable(null);
            controlsWindow.showAtLocation(decor, Gravity.NO_GRAVITY, 0, 0);
            jlog("controls overlay shown " + decor.getWidth() + "x" + decor.getHeight());
        } catch (Throwable t) { jlog("controls overlay unavailable: " + t); }
    }


    @Override
    protected void onCreate(Bundle state) {
        final Thread.UncaughtExceptionHandler prev = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((th, ex) -> {
            try { File d = getExternalFilesDir(null); if (d != null) { java.io.PrintWriter w = new java.io.PrintWriter(new File(d, "java_crash.txt")); ex.printStackTrace(w); w.close();
                    try (java.io.BufferedReader br = new java.io.BufferedReader(new java.io.FileReader(new File(d, "game_dir.txt")))) { String gd = br.readLine(); if (gd != null && !gd.isEmpty()) { java.io.PrintWriter w2 = new java.io.PrintWriter(new File(gd, "aot_java_crash.txt")); ex.printStackTrace(w2); w2.close(); } } catch (Throwable ignored2) { } } } catch (Throwable ignored) { }
            if (prev != null) prev.uncaughtException(th, ex);
        });
        super.onCreate(state);
        try { getWindow().getDecorView().post(this::addControls); } catch (Throwable t) { android.util.Log.w("aotgame", "controls overlay skipped: " + t); }
        boolean bundled = false;
        try (InputStream in = getAssets().open("game.exe")) { bundled = true; } catch (Exception ignored) { }
        bundledMode = bundled;
        if (state == null && !bundled) {
            showFolderPrompt();
        }
    }

    // ---- bundled-game mode: run in place from a user-selected folder (no copying) ----
    private boolean bundledMode, gateOpen;

    /* Tell the guest a pad is attached as soon as one is: it decides whether to offer controller support
       on its very first XInput query, long before the stick is touched. */
    private void scanPads() {
        boolean any = false;
        int[] ids = android.view.InputDevice.getDeviceIds();
        for (int id : ids) {
            android.view.InputDevice d = android.view.InputDevice.getDevice(id);
            if (d != null && (d.getSources() & android.view.InputDevice.SOURCE_GAMEPAD) != 0
                    || d != null && (d.getSources() & android.view.InputDevice.SOURCE_JOYSTICK) != 0) any = true;
        }
        int v = any ? 1 : 0;
        if (v == padPresent) return;
        padPresent = v;
        if (!nativesOk) return;
        try { nativePadPresent0(v); }
        catch (Throwable t) { nativesOk = false; android.util.Log.w("aotgame", "input natives unavailable: " + t); }
    }
    private boolean padListenerRegistered;

    @Override
    protected void onResume() {
        super.onResume();
        scanPads();
        if (!padListenerRegistered) {
            padListenerRegistered = true;
            android.hardware.input.InputManager im = (android.hardware.input.InputManager) getSystemService(INPUT_SERVICE);
            if (im != null) im.registerInputDeviceListener(new android.hardware.input.InputManager.InputDeviceListener() {
                @Override public void onInputDeviceAdded(int id) { scanPads(); }
                @Override public void onInputDeviceRemoved(int id) { scanPads(); }
                @Override public void onInputDeviceChanged(int id) { scanPads(); }
            }, null);
        }
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
