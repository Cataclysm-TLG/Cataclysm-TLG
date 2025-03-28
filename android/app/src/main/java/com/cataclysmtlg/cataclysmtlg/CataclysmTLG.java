package com.cataclysmtlg.cataclysmtlg;

import org.libsdl.app.SDLActivity;

import android.preference.PreferenceManager;
import android.util.Log;
import android.content.Context;
import android.os.Vibrator;
import android.view.View;
import android.widget.Toast;
import android.content.res.Configuration;

public class CataclysmTLG extends SDLActivity {
    private static final String TAG = "CTLG";

    private NativeUI nativeUI = new NativeUI(CataclysmTLG.this);

    public void vibrate(int duration) {
        try {
            Vibrator v = (Vibrator)getSystemService(Context.VIBRATOR_SERVICE);
            v.vibrate(duration);
        } catch(Exception e) {
            System.err.println(e.getMessage());
        }
    }

    public void toast(final String message) {
        try {
            runOnUiThread(new Runnable() {
                public void run() {
                    Toast.makeText(getApplicationContext(), message, Toast.LENGTH_SHORT).show();
                }
            });
        } catch(Exception e) {
            System.err.println(e.getMessage());
        }
    }

    private boolean isHardwareKeyboardAvailable() {
        return getResources().getConfiguration().keyboard == Configuration.KEYBOARD_QWERTY;
    }

    private float getDisplayDensity() {
        return getResources().getDisplayMetrics().density;
    }

    public void show_sdl_surface() {
        try {
            runOnUiThread(new Runnable() {
                public void run() {
                    mLayout.setVisibility(View.VISIBLE);
                }
            });
        } catch(Exception e) {
            System.err.println(e.getMessage());
        }
    }

    public boolean getDefaultSetting(final String settingsName, boolean defaultValue) {
        return PreferenceManager.getDefaultSharedPreferences(getApplicationContext()).getBoolean(settingsName, defaultValue);
    }

    public String getSystemLang() {
        return getResources().getConfiguration().locale.toLanguageTag().replace('-', '_');
    }

    public NativeUI getNativeUI() {
        return nativeUI;
    }
}
