// WebView probe: Chromium's renderer (a separate, isolated process on
// Android 11) must start under lxrun and FEX, run JavaScript and draw.
// Everything it learns goes to logcat under WebViewProbe:
//   "package <name> <version>"   the WebView implementation in use
//   "page finished"              the page loaded
//   "js <value>"                 JavaScript ran (a computed value)
//   "drawn <n> non-white pixels" the page reached the screen
package org.steamarm.webviewprobe;

import android.app.Activity;
import android.content.pm.PackageInfo;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.os.Bundle;
import android.os.Handler;
import android.util.Log;
import android.webkit.JavascriptInterface;
import android.webkit.WebView;
import android.webkit.WebViewClient;

public class MainActivity extends Activity {
    static final String TAG = "WebViewProbe";
    WebView web;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        PackageInfo p = WebView.getCurrentWebViewPackage();
        Log.i(TAG, "package " + (p == null ? "none" : p.packageName + " " + p.versionName));
        web = new WebView(this);
        web.getSettings().setJavaScriptEnabled(true);
        web.addJavascriptInterface(new Object() {
            @JavascriptInterface
            public void report(String value) { Log.i(TAG, "js " + value); }
        }, "probe");
        web.setWebViewClient(new WebViewClient() {
            @Override
            public void onPageFinished(WebView view, String url) {
                Log.i(TAG, "page finished");
                new Handler().postDelayed(MainActivity.this::countPixels, 3000);
            }
        });
        setContentView(web);
        String html = "<html><body style='margin:0;background:#1060c0'>"
                + "<h1 style='color:white;font-size:64px'>SteamARM WebView</h1>"
                + "<script>var s=0;for(var i=1;i<=1000;i++)s+=i;"
                + "probe.report('sum=' + s + ' ua=' + navigator.userAgent);</script>"
                + "</body></html>";
        web.loadDataWithBaseURL("https://probe.invalid/", html, "text/html", "utf-8", null);
    }

    void countPixels() {
        int w = web.getWidth(), h = web.getHeight();
        if (w <= 0 || h <= 0) { Log.i(TAG, "drawn 0 non-white pixels (no size)"); return; }
        Bitmap b = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
        web.draw(new Canvas(b));
        int n = 0;
        for (int y = 0; y < h; y += 8)
            for (int x = 0; x < w; x += 8)
                if ((b.getPixel(x, y) & 0xffffff) != 0xffffff) n++;
        Log.i(TAG, "drawn " + n + " non-white pixels (" + w + "x" + h + ", every 8th)");
    }
}
