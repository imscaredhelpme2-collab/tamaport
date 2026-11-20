@file:Suppress("FunctionName", "UNUSED_PARAMETER")

package com.tamaport.classic

import android.content.Context
import android.os.Build
import android.system.Os
import android.system.OsConstants
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log
import java.io.ByteArrayOutputStream
import java.io.Closeable
import java.io.InputStream
import java.io.File
import java.io.FileOutputStream
import java.io.OutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

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
    /** Reads the newest intact copy: the real file, or the previous good copy if the real one is missing/empty. */
    fun OnInputStreamOpenLocalFile(name: String): Int {
        val f = existingSave(name) ?: return -1
        return openIn { f.inputStream() }
    }
    /** Saves are written to a temporary file first and only swapped in when complete (see [commitSave]). */
    fun OnOutputStreamOpenLocalFile(name: String): Int = try {
        val tmp = File(ctx.filesDir, "$name.tmp")
        register(SafeOut(name, tmp, FileOutputStream(tmp)))
    } catch (e: Exception) { Log.e(TAG, "open out $name: $e"); -1 }

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
        val s: OutputStream = (streams[h] as? SafeOut)?.fos ?: streams[h] as? OutputStream ?: return -1
        return try { s.write(data); data.size } catch (e: Exception) { Log.e(TAG, "write: $e"); -1 }
    }

    fun OnReadFile(path: String, offset: Int, length: Int): ByteArray? = try {
        ctx.assets.open(path).buffered(8192).use { s -> s.skip(offset.toLong()); readUpTo(s, length) }
    } catch (e: Exception) { Log.e(TAG, "OnReadFile $path: $e"); null }

    fun OnReadGameData(name: String, size: Int): ByteArray? {
        val f = existingSave(name) ?: return null
        return try {
            val all = f.readBytes()
            if (size > 0 && size < all.size) all.copyOf(size) else if (size > all.size) all.copyOf(size) else all
        } catch (e: Exception) { Log.e(TAG, "OnReadGameData $name: $e"); null }
    }
    fun OnWriteGameData(name: String, data: ByteArray): Boolean = try {
        val tmp = File(ctx.filesDir, "$name.tmp")
        FileOutputStream(tmp).use { it.write(data); it.flush(); it.fd.sync() }
        commitSave(name, tmp)
    } catch (e: Exception) { Log.e(TAG, "OnWriteGameData $name: $e"); false }

    fun release() { textures.release(); sound.release(); streams.values.forEach { try { it.close() } catch (_: Exception) {} }; streams.clear() }

    // ---- helpers ----
    private fun register(c: Closeable): Int { val id = nextStream++; streams[id] = c; return id }
    private fun openIn(open: () -> InputStream): Int = try { register(open().buffered(8192)) } catch (e: Exception) { Log.d(TAG, "open failed: $e"); -1 }
    private fun closeStream(h: Int): Int {
        val s = streams.remove(h) ?: return -1
        return try {
            if (s is SafeOut) {
                try { s.fos.flush(); s.fos.fd.sync() } catch (e: Exception) { Log.w(TAG, "fsync ${s.name}: $e") }
                s.fos.close()
                if (commitSave(s.name, s.tmp)) 0 else -1
            } else { s.close(); 0 }
        } catch (e: Exception) { -1 }
    }

    private class SafeOut(val name: String, val tmp: File, val fos: FileOutputStream) : Closeable {
        override fun close() { fos.close() }
    }

    /** The real save, or its backup if the real one is missing or empty (e.g. after a power cut). */
    private fun existingSave(name: String): File? {
        for (n in arrayOf(name, "$name.bak")) {
            val f = File(ctx.filesDir, n)
            if (f.exists() && f.length() > 0) { if (n != name) logSave("RESTORED $name from backup copy"); return f }
        }
        return null
    }

    /**
     * Crash-safe save: the finished, flushed temp file replaces the real one with an atomic rename, and the previous
     * good copy is kept as <name>.bak. An empty write never replaces good data.
     */
    private fun commitSave(name: String, tmp: File): Boolean {
        val main = File(ctx.filesDir, name)
        val bak = File(ctx.filesDir, "$name.bak")
        if (!tmp.exists() || tmp.length() == 0L) { tmp.delete(); logSave("REFUSED empty write of $name"); return false }
        if (main.exists() && main.length() > 0) { bak.delete(); main.renameTo(bak) }
        val ok = tmp.renameTo(main)
        syncDirectory()
        logSave(if (ok) "saved $name (${main.length()} bytes)" else "RENAME FAILED for $name")
        return ok
    }

    private fun syncDirectory() {
        try {
            val fd = Os.open(ctx.filesDir.absolutePath, OsConstants.O_RDONLY, 0)
            try { Os.fsync(fd) } finally { Os.close(fd) }
        } catch (e: Exception) { /* best effort */ }
    }

    /** Small rolling log (files/save-log.txt) so lost saves can be diagnosed afterwards. */
    private fun logSave(msg: String) {
        Log.i(TAG, msg)
        try {
            val f = File(ctx.filesDir, "save-log.txt")
            if (f.exists() && f.length() > 16000) f.delete()
            f.appendText(SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date()) + "  " + msg + "\n")
        } catch (e: Exception) { /* ignore */ }
    }

    init {
        val files = ctx.filesDir.listFiles()?.filter { it.isFile && !it.name.startsWith("save-log") }?.joinToString { "${it.name}=${it.length()}B" }
        logSave("app started; files: ${files ?: "none"}")
    }

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
