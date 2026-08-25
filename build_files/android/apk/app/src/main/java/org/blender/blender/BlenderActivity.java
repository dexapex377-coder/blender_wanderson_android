/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

package org.blender.blender;

import android.app.NativeActivity;
import android.content.Context;
import android.content.Intent;
import android.content.res.AssetManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.system.Os;
import android.text.InputType;
import android.util.Log;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * NativeActivity subclass for Blender. Extracts the bundled runtime
 * (Python + scripts + datafiles) on first launch, then loads libblender.so
 * and bridges soft-keyboard IME text to native. Orientation follows the
 * device: see screenOrientation in the manifest.
 */
public class BlenderActivity extends NativeActivity {

  /* NativeActivity dlopen()s the library from native code, which never registers
   * it with the class loader, so the JNI lookup for the native methods below
   * fails with UnsatisfiedLinkError. Load it here as well to register it. */
  static {
    System.loadLibrary("blender");
  }

  /* Must match GHOST_SystemPathsAndroid: <filesDir>/blender/<version>. */
  private static final String VERSION = "5.3";
  /* Only a fallback now, for a payload built before package.sh started emitting
   * RUNTIME_REV. The real revision is a hash of the archive, so it changes by itself
   * whenever the packaged runtime does -- a hand-bumped constant was silently
   * forgotten, leaving edited Python scripts stranded in the APK while the device kept
   * running the copy it unpacked months earlier. */
  private static final String RUNTIME_REVISION = "py313-ui1-net3-pip-assets1";
  private static final String RUNTIME_ZIP = "blender_runtime.zip";
  private static final String RUNTIME_REV = "blender_runtime.rev";

  private static final String TAG = "blender";
  /* Must match build_files/android/deps/build.sh and package.sh. */
  private static final String PYTHON_VERSION = "3.13";
  /* Only used to fill in pyvenv.cfg; CPython does not validate it. */
  private static final String PYTHON_FULL_VERSION = "3.13.13";
  private static final String PYTHON_BIN_LIB = "libpython3_13_bin.so";

  private InputView inputView;

  private native void nativeOnCommitText(String text);
  private native void nativeOnKey(int keycode, int action, int metaState);

  @Override
  protected void onCreate(Bundle state) {
    /* Runtime files must exist before native Blender init reads them. */
    extractRuntimeIfNeeded();
    /* Must precede super.onCreate(): that is what starts the native thread,
     * and Blender reads both of these during its Python initialization. */
    setUpPythonInterpreter();
    super.onCreate(state);
    enterImmersive();
    requestAllFilesAccess();

    inputView = new InputView(this);
    addContentView(inputView, new ViewGroup.LayoutParams(1, 1));
  }

  /* Scoped storage confines the app to its sandbox, but Blender opens and saves
   * .blend files and their assets anywhere by path. Send the user to the "All
   * files access" screen once; it is a no-op after they grant it. */
  private void requestAllFilesAccess() {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager()) {
      return;
    }
    try {
      Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                 Uri.parse("package:" + getPackageName()));
      startActivity(intent);
    }
    catch (Exception ex) {
      /* Some devices lack the per-app screen; fall back to the global list. */
      try {
        startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
      }
      catch (Exception ignored) {
      }
    }
  }

  /* Hide the status/navigation bars so they don't overlap Blender's own menus
   * (the top File/Edit/… bar and the bottom timeline). Sticky immersive lets the
   * user swipe from an edge to reveal the bars temporarily. */
  private void enterImmersive() {
    View d = getWindow().getDecorView();
    d.setSystemUiVisibility(
        View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
        | View.SYSTEM_UI_FLAG_FULLSCREEN
        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
  }

  @Override
  public void onWindowFocusChanged(boolean hasFocus) {
    super.onWindowFocusChanged(hasFocus);
    /* Immersive mode is cleared when focus returns (e.g. after the soft keyboard
     * or a system dialog); re-apply it. */
    if (hasFocus) {
      enterImmersive();
    }
  }

  /**
   * Makes `sys.executable` real.
   *
   * Blender's extension system runs its CLI as a subprocess built from
   * `sys.executable`, so "Get Extensions" does nothing without an interpreter
   * Blender can execute. The interpreter ships in the native library directory
   * because the runtime payload lands in the app's data directory, which is
   * mounted noexec from API 29 on. Blender looks for it under
   * `<python>/bin/` (BKE_appdir_program_python_search), so link the two.
   *
   * The symlink is rebuilt on every launch: nativeLibraryDir contains a hash
   * that changes when the app is updated, which would leave it dangling.
   */
  private void setUpPythonInterpreter() {
    File root = new File(getFilesDir(), "blender/" + VERSION);
    File pythonHome = new File(root, "python");
    if (!pythonHome.isDirectory()) {
      /* Bootstrap profile without Python; nothing to wire up. */
      return;
    }
    try {
      File interpreter = new File(getApplicationInfo().nativeLibraryDir, PYTHON_BIN_LIB);
      if (interpreter.exists()) {
        File binDir = new File(pythonHome, "bin");
        binDir.mkdirs();
        File link = new File(binDir, "python" + PYTHON_VERSION);
        /* delete() rather than exists(): a dangling symlink reads as absent
         * but still makes symlink() fail with EEXIST. */
        link.delete();
        Os.symlink(interpreter.getAbsolutePath(), link.getAbsolutePath());
      }
      else {
        Log.w(TAG, "no bundled interpreter; online extensions will not work");
      }
      /* A child process gets none of Blender's Python configuration, so it
       * would compute its prefix from the interpreter's own location -- the
       * library directory, which holds no standard library. Blender's embedded
       * interpreter is unaffected: it runs an isolated config that ignores
       * PYTHONHOME and sets its home explicitly. */
      Os.setenv("PYTHONHOME", pythonHome.getAbsolutePath(), true);

      /* PYTHONHOME alone is not enough. bpy.app.python_args is ("-I",) unless
       * Blender was told to use the system environment, and the extension
       * system passes it, so the child starts in isolated mode -- which
       * implies -E and therefore ignores PYTHONHOME. It then resolves the
       * symlink above back to the library directory and finds no standard
       * library, dying with "Failed to import encodings module" before
       * running a line.
       *
       * pyvenv.cfg is the way out: CPython reads it as a file next to the
       * executable or one level up, which -E does not suppress -- that is
       * exactly how a virtualenv's symlinked interpreter finds its base. The
       * path is only known at runtime, so it is written here rather than
       * shipped in the payload. */
      writeText(new File(pythonHome, "pyvenv.cfg"),
          "home = " + new File(pythonHome, "bin").getAbsolutePath() + "\n"
              + "include-system-site-packages = true\n"
              + "version = " + PYTHON_FULL_VERSION + "\n");

      /* A child is a plain exec outside the app's linker namespace, so it
       * resolves "libcrypto.so" against the system paths and finds Android's
       * BoringSSL, which does not export the symbols the bundled _ssl module
       * needs -- the import then dies on OPENSSL_sk_pop_free. Naming the
       * library directory first puts the real OpenSSL ahead of it. The app's
       * own libraries are already loaded by this point (libblender.so is
       * loaded in the static initializer), so this only affects what comes
       * after: the extension system's subprocesses. */
      Os.setenv("LD_LIBRARY_PATH", getApplicationInfo().nativeLibraryDir, true);
    }
    catch (Exception ex) {
      /* Not fatal: everything except online extensions works without it. */
      Log.w(TAG, "python interpreter setup failed", ex);
    }
  }

  private static void writeText(File out, String text) throws Exception {
    try (OutputStream os = new FileOutputStream(out)) {
      os.write(text.getBytes("UTF-8"));
    }
  }

  /** Revision of the packaged runtime: a hash of the archive, written by package.sh. */
  private String runtimeRevision() {
    try (InputStream is = getAssets().open(RUNTIME_REV)) {
      ByteArrayOutputStream buf = new ByteArrayOutputStream();
      byte[] chunk = new byte[64];
      int n;
      while ((n = is.read(chunk)) > 0) {
        buf.write(chunk, 0, n);
      }
      String rev = buf.toString("UTF-8").trim();
      if (!rev.isEmpty()) {
        return rev;
      }
    }
    catch (Exception ex) {
      /* Payload predates the revision file; fall back to the constant. */
    }
    return RUNTIME_REVISION;
  }

  private void extractRuntimeIfNeeded() {
    File root = new File(getFilesDir(), "blender/" + VERSION);
    File marker = new File(root, ".installed-" + VERSION + "-" + runtimeRevision());
    if (marker.exists()) {
      return;
    }
    root.mkdirs();
    /* Drop markers from earlier revisions, so what is on disk stays readable at a
     * glance and they do not pile up one per build. */
    File[] stale = root.listFiles((dir, name) -> name.startsWith(".installed-"));
    if (stale != null) {
      for (File old : stale) {
        old.delete();
      }
    }
    try (InputStream is = getAssets().open(RUNTIME_ZIP, AssetManager.ACCESS_STREAMING);
         ZipInputStream zis = new ZipInputStream(is)) {
      ZipEntry e;
      byte[] buf = new byte[65536];
      while ((e = zis.getNextEntry()) != null) {
        File out = new File(root, e.getName());
        if (e.isDirectory()) {
          out.mkdirs();
          continue;
        }
        File parent = out.getParentFile();
        if (parent != null) {
          parent.mkdirs();
        }
        try (OutputStream os = new FileOutputStream(out)) {
          int n;
          while ((n = zis.read(buf)) > 0) {
            os.write(buf, 0, n);
          }
        }
      }
      marker.createNewFile();
    }
    catch (Exception ex) {
      throw new RuntimeException("Failed to extract Blender runtime", ex);
    }
  }

  /* Called from native (popupOnScreenKeyboard). */
  public void showKeyboard() {
    runOnUiThread(() -> {
      inputView.setFocusableInTouchMode(true);
      inputView.requestFocus();
      InputMethodManager imm = (InputMethodManager)getSystemService(Context.INPUT_METHOD_SERVICE);
      imm.showSoftInput(inputView, InputMethodManager.SHOW_IMPLICIT);
    });
  }

  /* Called from native (hideOnScreenKeyboard). */
  public void hideKeyboard() {
    runOnUiThread(() -> {
      InputMethodManager imm = (InputMethodManager)getSystemService(Context.INPUT_METHOD_SERVICE);
      imm.hideSoftInputFromWindow(inputView.getWindowToken(), 0);
    });
  }

  /** Invisible view whose InputConnection captures IME text. */
  private class InputView extends View {
    InputView(Context context) {
      super(context);
      setFocusable(true);
      setFocusableInTouchMode(true);
    }

    @Override
    public boolean onCheckIsTextEditor() {
      return true;
    }

    @Override
    public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
      outAttrs.inputType = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS;
      outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN;

      return new BaseInputConnection(this, false) {
        /* Text the IME is still composing -- the underlined word being typed.
         *
         * Soft keyboards do not send key events for ordinary characters. They
         * call setComposingText() once per keystroke with the whole word so
         * far, and only call commitText() when the word is finished: at a
         * space, at punctuation, or when a suggestion is tapped. Blender has no
         * concept of composing text, so the composition is mirrored into the
         * field as it grows and rewritten whenever it changes. Without this,
         * individual letters never arrive and only picking a suggestion types
         * anything -- while backspace still works, because that one *is*
         * delivered as a key event. */
        private String composing = "";

        /** Makes the field show `text` where it currently shows `composing`. */
        private void replaceComposing(String text) {
          int common = 0;
          final int max = Math.min(composing.length(), text.length());
          while (common < max && composing.charAt(common) == text.charAt(common)) {
            common++;
          }
          /* Never cut between the halves of a surrogate pair: sending one half
           * on its own would not be valid UTF-8 by the time it reaches GHOST. */
          if (common > 0 && common < text.length()
              && Character.isLowSurrogate(text.charAt(common)))
          {
            common--;
          }

          /* Backspace over the tail that no longer matches. Counted in code
           * points, since that is what one delete removes on Blender's side. */
          final int stale = composing.codePointCount(common, composing.length());
          for (int i = 0; i < stale; i++) {
            nativeOnKey(KeyEvent.KEYCODE_DEL, KeyEvent.ACTION_DOWN, 0);
            nativeOnKey(KeyEvent.KEYCODE_DEL, KeyEvent.ACTION_UP, 0);
          }
          if (common < text.length()) {
            nativeOnCommitText(text.substring(common));
          }
          composing = text;
        }

        @Override
        public boolean setComposingText(CharSequence text, int newCursorPosition) {
          replaceComposing(text.toString());
          return true;
        }

        @Override
        public boolean finishComposingText() {
          /* The composition became final as typed; it is already in the field. */
          composing = "";
          return true;
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
          /* Usually the composition unchanged, but autocorrect and suggestions
           * commit something different -- diffing covers both. */
          replaceComposing(text.toString());
          composing = "";
          return true;
        }

        @Override
        public boolean sendKeyEvent(KeyEvent event) {
          /* A key event outside the composition, so what was mirrored so far
           * stands on its own. */
          composing = "";
          nativeOnKey(event.getKeyCode(), event.getAction(), event.getMetaState());
          return true;
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
          composing = "";
          for (int i = 0; i < beforeLength; i++) {
            nativeOnKey(KeyEvent.KEYCODE_DEL, KeyEvent.ACTION_DOWN, 0);
            nativeOnKey(KeyEvent.KEYCODE_DEL, KeyEvent.ACTION_UP, 0);
          }
          return true;
        }
      };
    }
  }
}
