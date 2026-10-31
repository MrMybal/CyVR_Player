package com.cyvr.player;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.text.InputType;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.webkit.CookieManager;
import android.webkit.PermissionRequest;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import java.io.ByteArrayInputStream;
import java.net.URLEncoder;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;

/**
 * Web browser window. It runs in its own process because the system WebView
 * renders in-process on Quest: a failing page must not take the player with it.
 */
public class BrowserActivity extends Activity {
    private static final String HOME = "https://www.youtube.com/";
    private static final Set<String> PLAYABLE = new HashSet<>(Arrays.asList("m3u8", "mp4", "m4v", "webm", "mkv", "mov"));
    private WebView web;
    private EditText address;
    private ProgressBar progress;
    private Button back, forward, filter, play;
    private FrameLayout content;
    private View fullscreen;
    private WebChromeClient.CustomViewCallback fullscreenDone;
    private SharedPreferences preferences;
    private boolean french;
    private volatile boolean filtering;
    private volatile int blocked;
    private volatile String videoUrl, videoReferer;
    private volatile boolean videoIsStream;
    private boolean pageFailed, opening = true;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        french = "fr".equals(getIntent().getStringExtra("cyvr.language"));
        preferences = getSharedPreferences("browser", MODE_PRIVATE);
        filtering = preferences.getBoolean("filter", true);

        web = new WebView(this);
        WebSettings settings = web.getSettings();
        settings.setJavaScriptEnabled(true);
        settings.setDomStorageEnabled(true);
        settings.setMediaPlaybackRequiresUserGesture(false);
        settings.setAllowFileAccess(false);
        settings.setAllowContentAccess(false);
        settings.setMixedContentMode(WebSettings.MIXED_CONTENT_NEVER_ALLOW);
        settings.setSupportZoom(true);
        settings.setBuiltInZoomControls(true);
        settings.setDisplayZoomControls(false);
        settings.setUseWideViewPort(true);
        settings.setLoadWithOverviewMode(true);
        CookieManager.getInstance().setAcceptCookie(true);
        web.setWebViewClient(new PageClient());
        web.setWebChromeClient(new PageChrome());

        address = new EditText(this);
        address.setSingleLine(true);
        address.setTextColor(Color.WHITE);
        address.setHintTextColor(Color.GRAY);
        address.setHint(text("Search or enter an address", "Rechercher ou saisir une adresse"));
        address.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        address.setImeOptions(EditorInfo.IME_ACTION_GO);
        address.setOnEditorActionListener((view, action, event) -> {
            if (action != EditorInfo.IME_ACTION_GO && (event == null || event.getKeyCode() != KeyEvent.KEYCODE_ENTER))
                return false;
            navigate(address.getText().toString());
            ((InputMethodManager) getSystemService(INPUT_METHOD_SERVICE)).hideSoftInputFromWindow(address.getWindowToken(), 0);
            web.requestFocus();
            return true;
        });

        back = button("◀", view -> web.goBack());
        forward = button("▶", view -> web.goForward());
        Button reload = button("⟳", view -> web.reload());
        filter = button("", view -> toggleFilter());
        play = button(text("Play in CyVR", "Lire dans CyVR"), view -> playInCyVR());
        play.setBackgroundColor(Color.rgb(16, 106, 88));
        play.setVisibility(View.GONE);

        LinearLayout bar = new LinearLayout(this);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setBackgroundColor(Color.rgb(24, 34, 40));
        bar.setPadding(8, 8, 8, 8);
        bar.addView(back);
        bar.addView(forward);
        bar.addView(reload);
        bar.addView(address, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        bar.addView(play);
        bar.addView(filter);

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(100);
        content = new FrameLayout(this);
        content.addView(web);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.BLACK);
        root.addView(bar);
        root.addView(progress, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 6));
        root.addView(content, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        setContentView(root);

        showFilter();
        if (state != null)
            web.restoreState(state);
        else
            web.loadUrl(preferences.getString("page", HOME));
    }

    private String text(String english, String translated) {return french ? translated : english;}

    private Button button(String label, View.OnClickListener action) {
        Button view = new Button(this);
        view.setAllCaps(false);
        view.setText(label);
        view.setTextColor(Color.WHITE);
        view.setBackgroundColor(Color.rgb(34, 50, 59));
        view.setOnClickListener(action);
        LinearLayout.LayoutParams layout = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        layout.setMargins(4, 0, 4, 0);
        view.setLayoutParams(layout);
        return view;
    }

    private void navigate(String typed) {
        String value = typed.trim();
        if (value.isEmpty())
            return;
        if (value.startsWith("http://") || value.startsWith("https://"))
            web.loadUrl(value);
        else if (!value.contains(" ") && value.contains("."))
            web.loadUrl("https://" + value);
        else {
            try {
                web.loadUrl("https://duckduckgo.com/?q=" + URLEncoder.encode(value, "UTF-8"));
            } catch (java.io.UnsupportedEncodingException impossible) {
                throw new IllegalStateException(impossible);
            }
        }
    }

    private void toggleFilter() {
        filtering = !filtering;
        preferences.edit().putBoolean("filter", filtering).apply();
        showFilter();
        web.reload();
    }

    private void showFilter() {
        filter.setText(filtering ? text("Ads blocked: ", "Pubs bloquées : ") + blocked : text("Ads allowed", "Pubs autorisées"));
    }

    private void showNavigation(String url) {
        address.setText(url);
        back.setEnabled(web.canGoBack());
        forward.setEnabled(web.canGoForward());
    }

    private void playInCyVR() {
        final String url = videoUrl;
        if (url == null)
            return;
        web.evaluateJavascript("document.querySelectorAll('video').forEach(v=>v.pause())", null);
        Intent player = new Intent(Intent.ACTION_VIEW).setClassName(getPackageName(), "com.cyvr.player.PanelActivity")
            .setDataAndType(Uri.parse(url), "video/*")
            .putExtra("cyvr.title", web.getTitle())
            .putExtra("cyvr.referer", videoReferer)
            .putExtra("cyvr.agent", web.getSettings().getUserAgentString())
            .putExtra("cyvr.cookies", CookieManager.getInstance().getCookie(url));
        startActivity(player);
    }

    private void leaveFullscreen() {
        if (fullscreen == null)
            return;
        content.removeView(fullscreen);
        fullscreen = null;
        web.setVisibility(View.VISIBLE);
        if (fullscreenDone != null)
            fullscreenDone.onCustomViewHidden();
        fullscreenDone = null;
    }

    private void found(Uri url, String referer) {
        final String path = url.getLastPathSegment();
        final int dot = path == null ? -1 : path.lastIndexOf('.');
        final String type = dot < 0 ? "" : path.substring(dot + 1).toLowerCase(Locale.ROOT);
        if (!PLAYABLE.contains(type))
            return;
        final boolean stream = type.equals("m3u8");
        // The first playlist is the master; prefer it over any later variant or clip.
        if (videoIsStream && videoUrl != null)
            return;
        videoUrl = url.toString();
        videoReferer = referer;
        videoIsStream = stream;
        runOnUiThread(() -> play.setVisibility(View.VISIBLE));
    }

    private class PageClient extends WebViewClient {
        @Override public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
            if (filtering && AdFilter.blocks(request.getUrl())) {
                blocked++;
                runOnUiThread(BrowserActivity.this::showFilter);
                return new WebResourceResponse("text/plain", "utf-8", new ByteArrayInputStream(new byte[0]));
            }
            found(request.getUrl(), request.getRequestHeaders().get("Referer"));
            return null;
        }
        @Override public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
            // Pages must not launch other applications through intent: or market: links.
            final String scheme = request.getUrl().getScheme();
            return !"https".equals(scheme) && !"http".equals(scheme);
        }
        @Override public void onPageStarted(WebView view, String url, android.graphics.Bitmap icon) {
            videoUrl = null;
            videoReferer = null;
            videoIsStream = false;
            pageFailed = false;
            play.setVisibility(View.GONE);
            showNavigation(url);
        }
        @Override public void onReceivedError(WebView view, WebResourceRequest request, android.webkit.WebResourceError error) {
            if (!request.isForMainFrame())
                return;
            pageFailed = true;
            // A saved page that no longer loads must not trap every later start.
            if (opening && !HOME.equals(request.getUrl().toString()))
                view.post(() -> view.loadUrl(HOME));
        }
        @Override public void onPageFinished(WebView view, String url) {
            showNavigation(url);
            if (pageFailed)
                return;
            opening = false;
            preferences.edit().putString("page", url).apply();
        }
    }

    private class PageChrome extends WebChromeClient {
        @Override public void onProgressChanged(WebView view, int value) {
            progress.setProgress(value);
            progress.setVisibility(value < 100 ? View.VISIBLE : View.INVISIBLE);
        }
        @Override public void onShowCustomView(View view, CustomViewCallback done) {
            leaveFullscreen();
            fullscreen = view;
            fullscreenDone = done;
            web.setVisibility(View.GONE);
            content.addView(view, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }
        @Override public void onHideCustomView() {leaveFullscreen();}
        @Override public void onPermissionRequest(PermissionRequest request) {
            // Camera, microphone and other device access stay closed to web pages.
            if (Arrays.asList(request.getResources()).contains(PermissionRequest.RESOURCE_PROTECTED_MEDIA_ID))
                request.grant(new String[] {PermissionRequest.RESOURCE_PROTECTED_MEDIA_ID});
            else
                request.deny();
        }
    }

    @Override public void onBackPressed() {
        if (fullscreen != null)
            leaveFullscreen();
        else if (web.canGoBack())
            web.goBack();
        else
            finish();
    }

    @Override protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state);
        web.saveState(state);
    }
    @Override protected void onPause() {
        super.onPause();
        web.onPause();
        CookieManager.getInstance().flush();
    }
    @Override protected void onResume() {
        super.onResume();
        web.onResume();
    }
    @Override protected void onDestroy() {
        web.destroy();
        super.onDestroy();
    }
}
