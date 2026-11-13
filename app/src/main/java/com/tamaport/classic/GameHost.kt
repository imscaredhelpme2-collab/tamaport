@file:Suppress("FunctionName", "UNUSED_PARAMETER")

package com.tamaport.classic

import android.content.Context
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log
import java.io.ByteArrayOutputStream
import java.io.Closeable
import java.io.InputStream
import java.io.OutputStream

/**
 * The Java side that the original engine calls back into. The C++ bridge finds these methods by name and JNI
 * signature (they are the former `af` interface of the original app), so names and parameter types must not change.
 * Every call arrives on the GL thread.
 */
class GameHost(private val activity: GameActivity) {
    private val ctx: Context = activity.applicationContext
    val textures = TextureManager(ctx)
    val sound = SoundManager(ctx)
    private val prefs = ctx.getSharedPreferences("tama_prefs", Context.MODE_PRIVATE)

    private val streams = HashMap<Int, Closeable>()
    private var nextStream = 1
    @Volatile private var alertResult = -1

    // ---- first run flag ----
    fun IsFirst(): Boolean = prefs.getBoolean(KEY_FIRST, true)
    fun ClearFirst() { prefs.edit().putBoolean(KEY_FIRST, false).apply() }

    // ---- removed online features (ads, Facebook, billing) ----
    fun GetFacebookStatus(): Int = 0
    fun HideAdnet() {}
    fun ShowAdnet() {}
    fun SetAdnetVisible(v: Int) {}
    fun IsShowAdnet(): Boolean = false
    fun OnFacebookLogin() {}
    fun OnFacebookLogout() {}
    fun OnSendFacebook(): Boolean = false
    fun onUpgradeAppButtonClicked() {}

    // ---- misc ----
    fun OnMessage(msg: String?) { Log.d(TAG, "engine: $msg") }
    fun OnSysError(msg: String?) { activity.fatal(msg ?: "Unknown error") }
    fun OnGetSystemTime(): Int = (System.currentTimeMillis() % 2147483647L).toInt()

    fun OnSetVibrator(ms: Int) {
        try {
            val v: Vibrator = if (Build.VERSION.SDK_INT >= 31) (ctx.getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as VibratorManager).defaultVibrator
                              else @Suppress("DEPRECATION") (ctx.getSystemService(Context.VIBRATOR_SERVICE) as Vibrator)
            v.vibrate(VibrationEffect.createOneShot(ms.toLong().coerceAtLeast(1), VibrationEffect.DEFAULT_AMPLITUDE))
        } catch (e: Exception) { Log.w(TAG, "vibrate: $e") }
    }

    // ---- alerts & web view (drawn by the Android UI, not the engine) ----
    fun OnSetAlertView(title: String?, message: String?, positive: String?, negative: String?): Int {
        alertResult = -1
        activity.showAlert(title, message, positive, negative) { alertResult = it }
        return 0
    }
    fun OnUpdateAlertView(): Int = alertResult
    fun OnClearAlertView() {}
    fun OnSetWebView(page: String, x: Float, y: Float, w: Float, h: Float): Int { activity.showWebView(page, x, y, w, h); return -1 }
    fun OnClearWebView() { activity.hideWebView() }

    // ---- screenshot / share ----
    fun OnStartCapture(i: Int) { activity.requestCapture() }
    fun OnSelectSNS(): Boolean { activity.showSaveDialog(); return true }

    // ---- alarm ----
    fun OnStartAlarm(ms: Int) { AlarmLogic.scheduleIn(ctx, ms) }
    fun OnStopAlarm() { AlarmLogic.cancelAll(ctx) }

    // ---- sound ----
    fun OnPlaySound(i: Int) { sound.play(i) }
    fun OnStopSound(i: Int) { sound.stop(i) }
    fun OnTestSound(i: Int): Boolean = sound.isPlaying(i)
    fun OnSetVolume(v: Float) { sound.setVolume(v) }
    fun OnSetBgmVolume(v: Float) { sound.setBgmVolume(v) }
    fun OnSetSeVolume(v: Float) { sound.setSeVolume(v) }

    // ---- textures ----
    fun OnLoadTexture(name: String): Int = if (name.startsWith("@")) textures.loadText(name.substring(1)) else textures.loadFile(name)
    fun OnLoadTextTexture(text: String): Int = textures.loadText(text)
    fun OnGetTextureID(h: Int): Int = textures.glId(h)
    fun OnGetTextureWidth(h: Int): Int = textures.width(h)
    fun OnGetTextureHeight(h: Int): Int = textures.height(h)
    fun OnRemoveTexture(h: Int): Boolean = textures.remove(h)
    fun OnDrawTextTexture(h: Int, text: String, x: Int, y: Int, color: Int, size: Int): Boolean = textures.drawText(h, text, x, y, color, size)
    fun OnGetReloadCounter(): Int = textures.reloadCounter
    /** Packed as (height shl 16) or width, like the original. */
    fun OnMeasureString(text: String): Int {
        val p = android.graphics.Paint().apply { textSize = 16f }
        val (w, h) = TextureManager.measure(text, p)
        return (h shl 16) or w
    }

    // ---- files and streams ----
    fun OnInputStreamOpenResourceFile(path: String): Int = openIn { ctx.assets.open(path) }
    fun OnInputStreamOpenLocalFile(name: String): Int = openIn { ctx.openFileInput(name) }
    fun OnOutputStreamOpenLocalFile(name: String): Int = try { register(ctx.openFileOutput(name, Context.MODE_PRIVATE)) } catch (e: Exception) { Log.e(TAG, "open out $name: $e"); -1 }

    fun OnInputStreamRead(h: Int, n: Int): ByteArray? {
        val s = streams[h] as? InputStream ?: return null
        return try { readUpTo(s, n) } catch (e: Exception) { Log.e(TAG, "read: $e"); null }
    }
    fun OnInputStreamSkip(h: Int, n: Int): Int {
        val s = streams[h] as? InputStream ?: return -1
        return try { s.skip(n.toLong()).toInt() } catch (e: Exception) { -1 }
    }
    fun OnInputStreamClose(h: Int): Int = closeStream(h)
    fun OnOutputStreamClose(h: Int): Int = closeStream(h)
    fun OnOutputStreamWrite(h: Int, data: ByteArray): Int {
        val s = streams[h] as? OutputStream ?: return -1
        return try { s.write(data); data.size } catch (e: Exception) { Log.e(TAG, "write: $e"); -1 }
    }

    fun OnReadFile(path: String, offset: Int, length: Int): ByteArray? = try {
        ctx.assets.open(path).buffered(8192).use { s -> s.skip(offset.toLong()); readUpTo(s, length) }
    } catch (e: Exception) { Log.e(TAG, "OnReadFile $path: $e"); null }

    fun OnReadGameData(name: String, size: Int): ByteArray? {
        val f = ctx.getFileStreamPath(name)
        if (!f.exists()) return null
        return try {
            val all = f.readBytes()
            if (size > 0 && size < all.size) all.copyOf(size) else if (size > all.size) all.copyOf(size) else all
        } catch (e: Exception) { Log.e(TAG, "OnReadGameData $name: $e"); null }
    }
    fun OnWriteGameData(name: String, data: ByteArray): Boolean = try {
        ctx.openFileOutput(name, Context.MODE_PRIVATE).use { it.write(data) }; true
    } catch (e: Exception) { Log.e(TAG, "OnWriteGameData $name: $e"); false }

    fun release() { textures.release(); sound.release(); streams.values.forEach { try { it.close() } catch (_: Exception) {} }; streams.clear() }

    // ---- helpers ----
    private fun register(c: Closeable): Int { val id = nextStream++; streams[id] = c; return id }
    private fun openIn(open: () -> InputStream): Int = try { register(open().buffered(8192)) } catch (e: Exception) { Log.d(TAG, "open failed: $e"); -1 }
    private fun closeStream(h: Int): Int { val s = streams.remove(h) ?: return -1; return try { s.close(); 0 } catch (e: Exception) { -1 } }

    /** Reads up to n bytes (n <= 0 means to the end of the stream), like the original `OnInputStreamRead`. */
    private fun readUpTo(s: InputStream, n: Int): ByteArray {
        val out = ByteArrayOutputStream()
        val buf = ByteArray(8192)
        var left = n
        while (true) {
            val want = if (left > 0) minOf(buf.size, left) else buf.size
            val got = s.read(buf, 0, want)
            if (got < 0) break
            out.write(buf, 0, got)
            if (left > 0) { left -= got; if (left <= 0) break }
        }
        return out.toByteArray()
    }

    companion object {
        private const val TAG = "GameHost"
        private const val KEY_FIRST = "first_execution"
    }
}
