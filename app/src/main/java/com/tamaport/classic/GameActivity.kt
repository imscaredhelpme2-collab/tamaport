package com.tamaport.classic

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.ContentValues
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Color
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.MediaStore
import android.util.Log
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.FrameLayout
import android.widget.Toast
import java.io.File

/** Hosts the GL view and the native Android UI the engine asks for (dialogs, the Terms web page, share sheet). */
class GameActivity : Activity() {
    private lateinit var root: FrameLayout
    private var host: GameHost? = null
    private var gameView: GameView? = null
    private var webView: WebView? = null
    private var vpLeft = 0; private var vpTop = 0; private var vpW = 0; private var vpH = 0
    private var fatalShown = false
    private var captureFile: File? = null
    private var backCallback: Any? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (Build.VERSION.SDK_INT >= 28) window.attributes.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }
        setContentView(root)
        enterImmersive()

        val lib = try { assets.open(ENGINE_ASSET).use { it.readBytes() } } catch (e: Exception) { null }
        if (lib == null) { fatal(getString(R.string.missing_engine)); return }
        val h = GameHost(this)
        val err = NativeEngine.nativeCreate(h, lib)
        if (err != null) { fatal("Cannot load the game engine: $err"); return }
        host = h
        val v = GameView(this, h)
        gameView = v
        root.addView(v, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        registerBack()
    }

    // ---------- lifecycle ----------
    override fun onResume() {
        super.onResume()
        enterImmersive()
        gameView?.let {
            it.onResume(); it.markResume()
            AlarmLogic.cancelAll(this); AlarmLogic.deleteInfo(this)      // the pet is no longer "calling" once the app is open
        }
    }

    override fun onPause() {
        val v = gameView
        val finishing = isFinishing
        if (v != null) {
            val app = applicationContext
            v.queueEvent {
                AlarmLogic.deleteInfo(app)
                NativeEngine.nativeMakeAlarmInfo()
                AlarmLogic.process(app)
                if (finishing) { NativeEngine.nativeTerm(); NativeEngine.nativeStop() }
            }
            v.onPause()      // runs queued events first, then stops the GL thread
        }
        host?.sound?.pauseAll()
        super.onPause()
    }

    override fun onDestroy() {
        if (isFinishing) { host?.release(); if (host != null) NativeEngine.nativeDestroy(); host = null }
        super.onDestroy()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) enterImmersive()
    }

    @Suppress("DEPRECATION")
    private fun enterImmersive() {
        if (Build.VERSION.SDK_INT >= 30) {
            window.setDecorFitsSystemWindows(false)
            window.insetsController?.let {
                it.hide(WindowInsets.Type.systemBars())
                it.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_LAYOUT_STABLE or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_FULLSCREEN or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY)
        }
    }

    // ---------- back button ----------
    private fun registerBack() {
        if (Build.VERSION.SDK_INT >= 33) {
            val cb = android.window.OnBackInvokedCallback { onBack() }
            onBackInvokedDispatcher.registerOnBackInvokedCallback(android.window.OnBackInvokedDispatcher.PRIORITY_DEFAULT, cb)
            backCallback = cb
        }
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
        if (keyCode == KeyEvent.KEYCODE_BACK && backCallback == null) { onBack(); return true }
        return super.onKeyDown(keyCode, event)
    }

    /** Same behaviour as the original: inside the game, Back goes to the previous screen; on the title it asks to exit. */
    private fun onBack() {
        val v = gameView ?: run { finish(); return }
        v.queueEvent {
            val part = NativeEngine.nativeGetPart()
            if (part != 0) NativeEngine.nativeSetAppRequest(3)
            else runOnUiThread {
                AlertDialog.Builder(this).setMessage(R.string.dialog_end_app).setCancelable(false)
                    .setPositiveButton(R.string.dialog_yes) { _, _ -> finish() }
                    .setNegativeButton(R.string.dialog_no, null).show()
            }
        }
    }

    // ---------- UI requested by the engine ----------
    fun setGameViewport(left: Int, top: Int, w: Int, h: Int) { vpLeft = left; vpTop = top; vpW = w; vpH = h }

    fun showAlert(title: String?, message: String?, positive: String?, negative: String?, result: (Int) -> Unit) {
        runOnUiThread {
            if (isFinishing) return@runOnUiThread
            val b = AlertDialog.Builder(this).setCancelable(false)
            if (title != null) b.setTitle(title)
            if (message != null) b.setMessage(message)
            if (positive != null) b.setPositiveButton(positive) { _, _ -> result(0) }
            if (negative != null) b.setNegativeButton(negative) { _, _ ->
                result(1)
                if (message?.contains("notification", ignoreCase = true) == true) requestNotificationPermission()
            }
            b.show()
        }
    }

    private fun requestNotificationPermission() {
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQ_NOTIFY)
    }

    /** The engine gives positions in its 640-wide drawing units; map them onto the letterboxed game viewport. */
    fun showWebView(page: String, x: Float, y: Float, w: Float, h: Float) {
        runOnUiThread {
            if (isFinishing) return@runOnUiThread
            removeWebView()
            if (page.startsWith("http://") || page.startsWith("https://")) {
                startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(page))); return@runOnUiThread
            }
            val scale = vpW / 640f
            val wv = WebView(this)
            @Suppress("SetJavaScriptEnabled")
            wv.settings.javaScriptEnabled = true
            wv.isVerticalScrollBarEnabled = false
            wv.isHorizontalScrollBarEnabled = false
            wv.webViewClient = object : WebViewClient() {
                override fun shouldOverrideUrlLoading(view: WebView, url: String): Boolean {
                    startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url))); return true
                }
            }
            val lp = FrameLayout.LayoutParams(if (w > 0f) (w * scale).toInt() else vpW, (h * scale).toInt(), Gravity.TOP or Gravity.START)
            lp.leftMargin = vpLeft + (x * scale).toInt()
            lp.topMargin = vpTop + (y * scale).toInt()
            root.addView(wv, lp)
            wv.loadUrl("file:///android_asset/$page.html")
            webView = wv
        }
    }

    fun hideWebView() { runOnUiThread { removeWebView() } }
    private fun removeWebView() { webView?.let { root.removeView(it); it.destroy() }; webView = null }

    fun fatal(message: String) {
        runOnUiThread {
            if (fatalShown || isFinishing) return@runOnUiThread
            fatalShown = true
            AlertDialog.Builder(this).setMessage(message).setCancelable(false)
                .setPositiveButton(R.string.dialog_close) { _, _ -> finish() }.show()
        }
    }

    // ---------- screenshots ----------
    fun requestCapture() { gameView?.requestCapture() }

    /** Called on the GL thread with an 8-byte (w,h) header followed by RGBA pixels. */
    fun onCaptured(data: ByteArray?) {
        if (data == null || data.size < 8) return
        try {
            val w = ((data[0].toInt() and 0xFF) shl 24) or ((data[1].toInt() and 0xFF) shl 16) or ((data[2].toInt() and 0xFF) shl 8) or (data[3].toInt() and 0xFF)
            val h = ((data[4].toInt() and 0xFF) shl 24) or ((data[5].toInt() and 0xFF) shl 16) or ((data[6].toInt() and 0xFF) shl 8) or (data[7].toInt() and 0xFF)
            val px = IntArray(w * h)
            for (i in px.indices) {
                val o = 8 + i * 4
                px[i] = (0xFF shl 24) or ((data[o].toInt() and 0xFF) shl 16) or ((data[o + 1].toInt() and 0xFF) shl 8) or (data[o + 2].toInt() and 0xFF)
            }
            val bmp = Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888)
            val f = File(filesDir, CAPTURE_NAME)
            f.outputStream().use { bmp.compress(Bitmap.CompressFormat.PNG, 100, it) }
            captureFile = f
            bmp.recycle()
        } catch (e: Exception) { Log.e(TAG, "capture failed: $e") }
    }

    fun showSaveDialog() {
        runOnUiThread {
            if (isFinishing) return@runOnUiThread
            AlertDialog.Builder(this).setTitle(R.string.picture)
                .setItems(arrayOf(getString(R.string.save_to_gallery), getString(R.string.dialog_cancel))) { _, which -> if (which == 0) saveToGallery() }
                .show()
        }
    }

    private fun saveToGallery() {
        val src = captureFile ?: File(filesDir, CAPTURE_NAME)
        if (!src.exists()) { Toast.makeText(this, R.string.save_failed, Toast.LENGTH_SHORT).show(); return }
        try {
            val name = "Tamagotchi_${System.currentTimeMillis()}.png"
            if (Build.VERSION.SDK_INT >= 29) {
                val cv = ContentValues().apply {
                    put(MediaStore.Images.Media.DISPLAY_NAME, name); put(MediaStore.Images.Media.MIME_TYPE, "image/png")
                    put(MediaStore.Images.Media.RELATIVE_PATH, "Pictures/Tamagotchi"); put(MediaStore.Images.Media.IS_PENDING, 1)
                }
                val uri = contentResolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, cv) ?: throw IllegalStateException("insert failed")
                contentResolver.openOutputStream(uri)!!.use { out -> src.inputStream().use { it.copyTo(out) } }
                cv.clear(); cv.put(MediaStore.Images.Media.IS_PENDING, 0); contentResolver.update(uri, cv, null, null)
            } else {
                @Suppress("DEPRECATION")
                MediaStore.Images.Media.insertImage(contentResolver, src.absolutePath, name, null)
            }
            Toast.makeText(this, R.string.saved_ok, Toast.LENGTH_SHORT).show()
        } catch (e: Exception) {
            Log.e(TAG, "save to gallery: $e")
            Toast.makeText(this, R.string.save_failed, Toast.LENGTH_SHORT).show()
        }
    }

    companion object {
        private const val TAG = "GameActivity"
        private const val ENGINE_ASSET = "engine/engine.bin"
        private const val CAPTURE_NAME = "Tamago_CaptureImage.png"
        private const val REQ_NOTIFY = 7
    }
}
