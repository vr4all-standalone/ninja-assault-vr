// hotd2-vr: modified in 2026 by mikermak for the Quest VR mode (see "git log master..hotd2-vr").
package com.flycast.emulator;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.Configuration;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.preference.PreferenceManager;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.ViewConfiguration;
import android.view.Window;
import android.view.WindowManager;
import android.window.OnBackInvokedDispatcher;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.core.app.ActivityCompat;

import com.flycast.emulator.config.Config;
import com.flycast.emulator.emu.AudioBackend;
import com.flycast.emulator.emu.HttpClient;
import com.flycast.emulator.emu.JNIdc;
import com.flycast.emulator.periph.InputDeviceManager;
import com.flycast.emulator.periph.SipEmulator;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

import static android.content.res.Configuration.HARDKEYBOARDHIDDEN_NO;
import static android.view.View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR;
import static android.view.WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS;
import static android.view.WindowManager.LayoutParams.FLAG_TRANSLUCENT_NAVIGATION;
import static android.view.WindowManager.LayoutParams.FLAG_TRANSLUCENT_STATUS;

public abstract class BaseGLActivity extends Activity implements ActivityCompat.OnRequestPermissionsResultCallback {
    private static final int STORAGE_PERM_REQUEST = 1001;
    private static final int AUDIO_PERM_REQUEST = 1002;

    protected SharedPreferences prefs;
    private AudioBackend audioBackend;
    protected Handler handler = new Handler();
    private boolean audioPermissionRequested = false;
    private boolean storagePermissionGranted = false;
    private boolean paused = true;
    private boolean resumedCalled = false;
    private String pendingIntentUrl;
    private boolean hasKeyboard = false;
    private AndroidStorage storage;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        if (!(getApplicationContext() instanceof Emulator)) {
            // app is in restricted mode, kill it
            // See https://medium.com/@pablobaxter/what-happened-to-my-subclass-android-application-924c91bafcac
            handler.post(() -> System.exit(0));
            finish();
            return;
        }
        // Set the navigation bar color to 0 to avoid left over when it fades out on Android 10
        Window window = getWindow();
        window.addFlags(FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
        window.clearFlags(FLAG_TRANSLUCENT_STATUS | FLAG_TRANSLUCENT_NAVIGATION);
        window.setNavigationBarColor(0);
        window.getDecorView().setSystemUiVisibility(SYSTEM_UI_FLAG_LIGHT_STATUS_BAR);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            getWindow().getAttributes().layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        }

        if (!getFilesDir().exists()) {
            getFilesDir().mkdir();
        }
        prefs = PreferenceManager.getDefaultSharedPreferences(this);

        Emulator.setCurrentActivity(this);
        new HttpClient().nativeInit();

        String homeDir = prefs.getString(Config.pref_home, "");
        // Check that home dir is valid
        if (homeDir.isEmpty()) {
            // home dir not set: use default
            homeDir = getDefaultHomeDir();
            prefs.edit().putString(Config.pref_home, homeDir).apply();
        }

        String result = JNIdc.initEnvironment((Emulator)getApplicationContext(), getFilesDir().getAbsolutePath(), homeDir,
                Locale.getDefault().toString());
        if (result != null) {
            showAlertDialog(result);
            return;
        }
        Log.i("flycast", "Environment initialized");
        copyBundledModels(homeDir);
        Emulator app = (Emulator)getApplicationContext();
        app.getConfigurationPrefs();
        storage = new AndroidStorage(this);
        setStorageDirectories();

        boolean externalStorageLegacy = true;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            externalStorageLegacy = Environment.isExternalStorageLegacy();
            //Log.i("flycast", "External storage legacy: " + (externalStorageLegacy ? "preserved" : "lost"));
        }
        if (!storagePermissionGranted) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M || !externalStorageLegacy)
                // No permission needed before Android 6
                // Permissions only needed in legacy external storage mode
                storagePermissionGranted = true;
            else {
                Log.i("flycast", "Asking for external storage permission");
                ActivityCompat.requestPermissions(this,
                        new String[]{
                                Manifest.permission.READ_EXTERNAL_STORAGE,
                                Manifest.permission.WRITE_EXTERNAL_STORAGE
                        },
                        STORAGE_PERM_REQUEST);
            }
        }

        Log.i("flycast", "Initializing input devices");
        InputDeviceManager.getInstance().startListening(getApplicationContext());
        register(this);
        audioBackend = new AudioBackend();
        onConfigurationChanged(getResources().getConfiguration());

        // Back button handling on Android 13+
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, () -> {
                onBackKey();
            });
        }

        // When viewing a resource, pass its URI to the native code for opening
        Intent intent = getIntent();
        // hotd2-vr: settings for this run from the launch (adb shell am start ... --es
        // hotd2vr.config "network:GGPO=yes,..."), as -config on the PC; a test aid
        String launchConfig = intent != null ? intent.getStringExtra("hotd2vr.config") : null;
        if (launchConfig != null && !launchConfig.isEmpty())
            JNIdc.setConfigOverrides(launchConfig);
        if (getPackageName().equals(VR_APPLICATION_ID)) {
            // Ninja's standalone entry always checks the original set, including ACTION_VIEW
            // and adb launches. File intents must not bypass the setup panel's validation.
            String game = findVrGame();
            Log.i("flycast", game != null ? "VR build: booting " + game : "VR build: no game found, to the start screen");
            if (game == null) {
                showVrSetup();
                return;
            }
            if (storagePermissionGranted)
                JNIdc.setGameUri(game);
            else
                pendingIntentUrl = game;
        }
        else if (intent != null && Intent.ACTION_VIEW.equals(intent.getAction())) {
            Uri gameUri = intent.getData();
            // Flush the intent to prevent multiple calls
            getIntent().setData(null);
            setIntent(null);
            if (gameUri != null) {
                if (storagePermissionGranted)
                    JNIdc.setGameUri(gameUri.toString());
                else
                    pendingIntentUrl = gameUri.toString();
            }
        }
        Log.i("flycast", "BaseGLActivity.onCreate done");
    }

    // hotd2-vr: the "vr" build type's applicationId (build.gradle.kts, androidComponents)
    private static final String VR_APPLICATION_ID = "com.vrlightgunhub.ninjaassault";

	// Called from native code
    public void showAlertDialog(String message)
    {
        handler.post(new Runnable() {
            @Override
            public void run() {
                AlertDialog.Builder dlgAlert = new AlertDialog.Builder(BaseGLActivity.this);
                dlgAlert.setMessage(getResources().getString(R.string.init_failed_message) + message);
                dlgAlert.setTitle(R.string.flycast_error);
                dlgAlert.setPositiveButton(R.string.exit,
                        new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface dialog, int id) {
                                BaseGLActivity.this.finish();
                            }
                        });
                dlgAlert.setIcon(android.R.drawable.ic_dialog_alert);
                dlgAlert.setCancelable(false);
                dlgAlert.create().show();
            }
        });
    }

    // The setup panel and explicit file/adb launches share the same original-revision checks.
    private String findVrGame()
    {
        Intent intent = getIntent();
        String picked = intent != null ? intent.getStringExtra(NinjaStartActivity.EXTRA_GAME) : null;
        if (intent != null && Intent.ACTION_VIEW.equals(intent.getAction())) {
            Uri uri = intent.getData();
            if (uri == null || uri.getPath() == null || (uri.getScheme() != null && !"file".equals(uri.getScheme())))
                return null;
            picked = uri.getPath();
        }
        return NinjaFiles.launchPath(picked != null ? new File(picked) : null, NinjaStartActivity.roots(this));
    }
    // hotd2-vr: the start screen, when the game can't start yet
    private void showVrSetup()
    {
        quitToStart(this);
    }

    /**
     * hotd2-vr: back to Ninja Assault VR's start screen (NinjaStartActivity): it opens in the
     * headset's home (an immersive app opens a panel that way), and the game's activity finishes.
     * The VR menu's "Quit to the start screen" calls this through JNI, from any thread.
     */
    public static void quitToStart(Activity activity)
    {
        activity.runOnUiThread(() -> {
            Intent panel = new Intent(activity.getApplicationContext(), NinjaStartActivity.class);
            panel.putExtra(NinjaStartActivity.EXTRA_SUPPRESS_AUTO_START, true);
            panel.setAction(Intent.ACTION_MAIN);
            panel.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            try {
                android.app.PendingIntent pending = android.app.PendingIntent.getActivity(activity.getApplicationContext(),
                        0, panel, android.app.PendingIntent.FLAG_UPDATE_CURRENT | android.app.PendingIntent.FLAG_IMMUTABLE);
                Intent home = new Intent(Intent.ACTION_MAIN)
                        .addCategory(Intent.CATEGORY_HOME)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                        .putExtra("extra_launch_in_home_pending_intent", pending);
                activity.startActivity(home);
            } catch (Exception e) {
                Log.w("flycast", "Start screen through the home failed, opening it directly", e);
                activity.startActivity(panel);
            }
            activity.finish();
        });
    }

    // hotd2-vr: the models that come with the app (assets/models: guns/, controllers/ and CREDITS.txt, in
    // the vr build when hotd2-vr/models was there to build it with) copied to <home>/data/models, the
    // data folder's models/ where the native side looks for them last (get_writable_data_path("models"),
    // xr_own.cpp). Copied again only when the app was installed or updated since the last copy (its
    // versionName and install time, in models/.installed) or a file is missing; a new copy replaces the
    // old one whole, and an APK with none removes an old copy. Small (a few MB): on this thread, before
    // the native side looks.
    private void copyBundledModels(String homeDir)
    {
        File data = new File(homeDir, "data");
        File models = new File(data, "models");
        File stamp = new File(models, ".installed");
        try {
            List<String> files = new ArrayList<>();
            listAssets("models", files);
            if (files.isEmpty()) {
                if (stamp.exists()) {
                    deleteTree(models);
                    Log.i("flycast", "Bundled models: none in this app, the old copy removed");
                }
                return;
            }
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            String version = info.versionName + " " + info.lastUpdateTime;
            boolean current = stamp.exists() && version.equals(readText(stamp));
            for (String f : files)
                current = current && new File(data, f).isFile();
            if (current)
                return;
            deleteTree(models);
            byte[] buffer = new byte[65536];
            for (String f : files) {
                File target = new File(data, f);
                File parent = target.getParentFile();
                if (parent != null && !parent.isDirectory() && !parent.mkdirs())
                    throw new IOException("cannot make " + parent);
                File part = new File(target.getPath() + ".part");
                try (InputStream in = getAssets().open(f); OutputStream out = new FileOutputStream(part)) {
                    int n;
                    while ((n = in.read(buffer)) > 0)
                        out.write(buffer, 0, n);
                }
                if (!part.renameTo(target))
                    throw new IOException("cannot rename " + part);
            }
            try (OutputStream out = new FileOutputStream(stamp)) {
                out.write(version.getBytes(StandardCharsets.UTF_8));
            }
            Log.i("flycast", "Bundled models: " + files.size() + " files copied to " + models);
        } catch (Exception e) {
            Log.w("flycast", "Bundled models: copying to " + models + " failed", e);
        }
    }

    // The files under an assets folder, as paths from the assets' root (an empty folder: none).
    private void listAssets(String path, List<String> out) throws IOException
    {
        String[] names = getAssets().list(path);
        if (names == null || names.length == 0) {
            if (path.contains("/"))
                out.add(path);
            return;
        }
        for (String name : names)
            listAssets(path + "/" + name, out);
    }

    private static String readText(File file) throws IOException
    {
        byte[] bytes = new byte[(int)Math.min(file.length(), 4096)];
        try (InputStream in = new FileInputStream(file)) {
            int at = 0, n;
            while (at < bytes.length && (n = in.read(bytes, at, bytes.length - at)) > 0)
                at += n;
            return new String(bytes, 0, at, StandardCharsets.UTF_8);
        }
    }

    private static void deleteTree(File file)
    {
        File[] children = file.listFiles();
        if (children != null)
            for (File child : children)
                deleteTree(child);
        file.delete();
    }

    private void setStorageDirectories()
    {
        String android_home_directory = Environment.getExternalStorageDirectory().getAbsolutePath();
        List<String> pathList = new ArrayList<>();
        pathList.add(android_home_directory);
        pathList.addAll(FileBrowser.getExternalMounts());
        pathList.add(getApplicationContext().getFilesDir().getAbsolutePath());
        File dir= getApplicationContext().getExternalFilesDir(null);
        if (dir != null)
            pathList.add(dir.getAbsolutePath());
        Log.i("flycast", "Storage dirs: " + pathList);
        if (storage != null)
            storage.setStorageDirectories(pathList);
        JNIdc.setExternalStorageDirectories(pathList.toArray());
    }

    // Testing
    public AndroidStorage getStorage() {
        return storage;
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        paused = true;
        InputDeviceManager.getInstance().stopListening();
        register(null);
        if (audioBackend != null)
            audioBackend.release();
        Emulator.setCurrentActivity(null);
        stopEmulator();
    }

    @Override
    protected void onPause() {
        super.onPause();
        resumedCalled = false;
        handleStateChange(true);
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumedCalled = true;
        handleStateChange(false);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            handleStateChange(false);
        } else {
            handleStateChange(true);
        }
    }

    @Override
    public void onConfigurationChanged(@NonNull Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        hasKeyboard = newConfig.hardKeyboardHidden == HARDKEYBOARDHIDDEN_NO;
    }

    protected abstract void doPause();
    protected abstract void doResume();
    protected abstract boolean isSurfaceReady();

    public void handleStateChange(boolean paused)
    {
        if (paused == this.paused)
            return;
        if (!paused && (!resumedCalled || !isSurfaceReady()))
            return;
        this.paused = paused;
        if (paused) {
            doPause();
        }
        else {
            doResume();
        }
    }

    private boolean showMenu() {
        JNIdc.guiOpenSettings();
        return true;
    }

    private boolean processJoystickInput(MotionEvent event, InputDevice.MotionRange range) {
        float v = event.getAxisValue(range.getAxis(), event.getActionIndex());
        // normalize to [-32768, 32767]
        v = (v - range.getMin()) / range.getRange() * 65535.f - 32768.f;
        return InputDeviceManager.getInstance().axisEvent(event.getDeviceId(), range.getAxis(), (int)Math.round(v));
    }
    @Override
    public boolean onGenericMotionEvent(MotionEvent event) {
        InputDeviceManager deviceManager = InputDeviceManager.getInstance();
        if ((event.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) == InputDevice.SOURCE_CLASS_JOYSTICK
                && event.getAction() == MotionEvent.ACTION_MOVE
                && event.getDevice() != null)
        {
            List<InputDevice.MotionRange> axes = event.getDevice().getMotionRanges();
            boolean rc = false;
            int actionPointerIndex = event.getActionIndex();
            for (InputDevice.MotionRange range : axes)
            {
                if ((range.getSource() & InputDevice.SOURCE_CLASS_MASK) != InputDevice.SOURCE_CLASS_JOYSTICK)
		            // Ignore mouse/touchpad axes (dualshock 4)
		            continue;
                if (range.getAxis() == MotionEvent.AXIS_HAT_X) {
                    float v = event.getAxisValue(MotionEvent.AXIS_HAT_X, actionPointerIndex);
                    if (v == -1.0) {
                        rc |= deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_LEFT, true);
                        deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_RIGHT, false);
                    }
                    else if (v == 1.0) {
                        deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_LEFT, false);
                        rc |= deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_RIGHT, true);
                    } else {
                        deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_LEFT, false);
                        deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_RIGHT, false);
                    }
                }
                else if (range.getAxis() == MotionEvent.AXIS_HAT_Y) {
                    float v = event.getAxisValue(MotionEvent.AXIS_HAT_Y, actionPointerIndex);
                    if (v == -1.0) {
                        rc |= deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_UP, true);
                        deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_DOWN, false);
                    }
                    else if (v == 1.0) {
                        deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_UP, false);
                        rc |= deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_DOWN, true);
                    } else {
                        deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_UP, false);
                        deviceManager.buttonEvent(event.getDeviceId(), KeyEvent.KEYCODE_DPAD_DOWN, false);
                    }
                }
                else {
                    rc |= processJoystickInput(event, range);
                }
            }
            if (rc)
                return true;
        }
        else if ((event.getSource() & InputDevice.SOURCE_CLASS_POINTER) == InputDevice.SOURCE_CLASS_POINTER) {
            switch (event.getAction()) {
                case MotionEvent.ACTION_SCROLL:
                    deviceManager.mouseScrollEvent(Math.round(-event.getAxisValue(MotionEvent.AXIS_VSCROLL)));
                    break;
                default:
                    deviceManager.mouseEvent(Math.round(event.getX()), Math.round(event.getY()), event.getButtonState());
                    break;
            }
            return true;
        }
        return super.onGenericMotionEvent(event);
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        InputDevice device = event.getDevice();
        if (QuestStartButtonPolicy.useOpenXr(getPackageName().equals(VR_APPLICATION_ID),
                device == null ? 0 : device.getVendorId(),
                device == null ? 0 : device.getSources(), event.getKeyCode())) {
            if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0)
                Log.i("flycast", "Quest Start button routed only to OpenXR");
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean onKeyUp(int keyCode, KeyEvent event) {
        if (InputDeviceManager.getInstance().buttonEvent(event.getDeviceId(), keyCode, false))
            return true;
        if (hasKeyboard && InputDeviceManager.getInstance().keyboardEvent(keyCode, false))
            return true;
        return super.onKeyUp(keyCode, event);
    }

    private void onBackKey() {
        if (JNIdc.guiIsContentBrowser()) {
            finish();
        }
        else {
            showMenu();
        }
    }

    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (event.getRepeatCount() == 0) {
            if (keyCode == KeyEvent.KEYCODE_BACK) {
                onBackKey();
                return true;
            }
            InputDeviceManager deviceManager = InputDeviceManager.getInstance();
            if (deviceManager.buttonEvent(event.getDeviceId(), keyCode, true))
                return true;

            if (hasKeyboard) {
                deviceManager.keyboardEvent(keyCode, true);
                if (!event.isCtrlPressed() && (event.isPrintingKey() || event.getKeyCode() == KeyEvent.KEYCODE_SPACE))
                    deviceManager.keyboardText(event.getUnicodeChar());
                return true;
            }
            if (ViewConfiguration.get(this).hasPermanentMenuKey()) {
                if (keyCode == KeyEvent.KEYCODE_MENU) {
                    return showMenu();
                }
            }
        }
        return super.onKeyDown(keyCode, event);
    }

    protected void stopEmulator() {
        JNIdc.stop();
    }

    void requestRecordAudioPermission() {
        if (audioPermissionRequested)
            return;
        audioPermissionRequested = true;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
            handler.post(new Runnable() {
                @Override
                public void run() {
                    Log.i("flycast", "Requesting Record audio permission");
                    ActivityCompat.requestPermissions(BaseGLActivity.this,
                            new String[]{
                                    Manifest.permission.RECORD_AUDIO
                            },
                            AUDIO_PERM_REQUEST);

                }
            });
        }
        else
        {
            onRequestPermissionsResult(AUDIO_PERM_REQUEST, new String[] { Manifest.permission.RECORD_AUDIO },
                    new int[] { PackageManager.PERMISSION_GRANTED });
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M)
            super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == AUDIO_PERM_REQUEST && permissions.length > 0
                && Manifest.permission.RECORD_AUDIO .equals(permissions[0]) && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
            Log.i("flycast", "Record audio permission granted");
            SipEmulator sip = new SipEmulator();
            JNIdc.setupMic(sip);
        }
        else if (requestCode == STORAGE_PERM_REQUEST) {
            Log.i("flycast", "External storage permission granted");
            storagePermissionGranted = true;
            setStorageDirectories();
            if (pendingIntentUrl != null) {
                JNIdc.setGameUri(pendingIntentUrl);
                pendingIntentUrl = null;
            }

            //setup mic
            if (InputDeviceManager.isMicPluggedIn())
                requestRecordAudioPermission();
        }

    }

    private String getDefaultHomeDir() {
        File dir = getExternalFilesDir(null);
        if (dir == null)
            dir = getFilesDir();
        return dir.getAbsolutePath();
    }

    // Called from native code
    public void onGameStateChange(boolean started) {
        runOnUiThread(new Runnable() {
            public void run() {
                if (started)
                    getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                else
                    getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        switch (requestCode)
        {
            case AndroidStorage.ADD_STORAGE_ACTIVITY_REQUEST:
                storage.onAddStorageResult(data);
                break;
            case AndroidStorage.IMPORT_HOME_ACTIVITY_REQUEST:
                storage.onImportHomeResult(data);
                break;
            case AndroidStorage.EXPORT_HOME_ACTIVITY_REQUEST:
                storage.onExportHomeResult(data);
                break;
        }
    }

    private static native void register(BaseGLActivity activity);
    
	public String getNativeLibDir() {
        return getApplicationContext().getApplicationInfo().nativeLibraryDir;
    }
    
	public String getInternalFilesDir() {
        return getFilesDir().getAbsolutePath();
    }
}
