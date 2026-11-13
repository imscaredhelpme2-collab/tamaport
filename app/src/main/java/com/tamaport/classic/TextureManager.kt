package com.tamaport.classic

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.opengl.GLES20
import android.opengl.GLUtils
import android.util.Log

/**
 * Port of the original shell's texture manager. Textures are identified to the engine by a 1-based handle;
 * the engine then asks for the real GL id. Everything here runs on the GL thread.
 */
class TextureManager(private val ctx: Context) {
    private abstract class Entry { var glId = 0; var w = 0; var h = 0 }
    private class FileEntry(val name: String) : Entry()
    private class TextEntry(val text: String) : Entry() {
        class Op(val text: String, val x: Int, val y: Int, val color: Int, val size: Int)
        val ops = ArrayList<Op>()
        var bmp: Bitmap? = null
    }

    private val entries = ArrayList<Entry?>()

    /** Incremented whenever the GL context was recreated and every texture was reloaded. */
    @Volatile var reloadCounter = 0
        private set

    fun loadFile(name: String): Int = add(FileEntry(name).also { createFile(it) })
    fun loadText(text: String): Int = add(TextEntry(text).also { createText(it) })

    fun glId(handle: Int): Int = entry(handle)?.glId ?: -1
    fun width(handle: Int): Int = entry(handle)?.w ?: -1
    fun height(handle: Int): Int = entry(handle)?.h ?: -1

    fun remove(handle: Int): Boolean {
        val e = entry(handle) ?: return false
        deleteGl(e)
        entries[handle - 1] = null
        return true
    }

    fun drawText(handle: Int, text: String, x: Int, y: Int, color: Int, size: Int): Boolean {
        val i = handle - 1
        if (i < 0 || i >= entries.size) return false
        val e = entries[i] as? TextEntry ?: return true
        e.ops.add(TextEntry.Op(text, x, y, color, size))
        val bmp = e.bmp ?: Bitmap.createBitmap(e.w, e.h, Bitmap.Config.ARGB_8888).also { e.bmp = it }
        drawOp(e.ops.last(), bmp)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, e.glId)
        GLUtils.texSubImage2D(GLES20.GL_TEXTURE_2D, 0, 0, 0, bmp)
        return true
    }

    /** The GL context was lost and recreated: every old id is invalid, so rebuild all textures. */
    fun onContextLost() {
        for (e in entries) {
            when (e) {
                is FileEntry -> createFile(e)
                is TextEntry -> { createText(e); e.ops.forEach { op -> e.bmp?.let { drawOp(op, it) } }; e.bmp?.let { upload(e, it) } }
                else -> {}
            }
        }
        reloadCounter++
    }

    fun release() {
        entries.forEach { if (it != null) deleteGl(it) }
        entries.clear()
    }

    // ---- internals ----
    private fun entry(handle: Int): Entry? = if (handle >= 1 && handle <= entries.size) entries[handle - 1] else null

    private fun add(e: Entry): Int {
        for (i in entries.indices) if (entries[i] == null) { entries[i] = e; return i + 1 }
        entries.add(e)
        return entries.size
    }

    private fun deleteGl(e: Entry) {
        if (e.glId != 0) GLES20.glDeleteTextures(1, intArrayOf(e.glId), 0)
        e.glId = 0
    }

    private fun upload(e: Entry, bmp: Bitmap) {
        val ids = IntArray(1)
        if (e.glId == 0) { GLES20.glGenTextures(1, ids, 0); e.glId = ids[0] }
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, e.glId)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        GLUtils.texImage2D(GLES20.GL_TEXTURE_2D, 0, bmp, 0)
        e.w = bmp.width; e.h = bmp.height
    }

    /** The shell strips the last 4 characters of the engine's name (".png") and tries ".tgd" then ".png". */
    private fun createFile(e: FileEntry) {
        deleteGl(e)
        val base = if (e.name.length >= 4) e.name.substring(0, e.name.length - 4) else e.name
        val opts = BitmapFactory.Options().apply { inScaled = false; inPreferredConfig = Bitmap.Config.ARGB_8888 }
        var bmp: Bitmap? = null
        for (ext in arrayOf(".tgd", ".png")) {
            bmp = try { ctx.assets.open(base + ext).use { BitmapFactory.decodeStream(it, null, opts) } } catch (ex: Exception) { null }
            if (bmp != null) break
        }
        if (bmp == null) { Log.e(TAG, "texture not found: ${e.name}"); e.w = 0; e.h = 0; return }
        upload(e, bmp)
        bmp.recycle()
    }

    private fun createText(e: TextEntry) {
        deleteGl(e)
        val paint = textPaint(16)
        val (tw, th) = measure(e.text, paint)
        val s = maxOf(tw, th)
        var size = 1
        while (size < s) size = size shl 1
        val bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        drawString(e.text, 0, 0, paint, Canvas(bmp))
        e.bmp = bmp
        upload(e, bmp)
    }

    private fun drawOp(op: TextEntry.Op, bmp: Bitmap) {
        val paint = textPaint(op.size)
        // The original drew into an ALPHA_8 bitmap, so only the colour's alpha mattered: draw white with that alpha.
        paint.color = Color.argb(Color.alpha(op.color).let { if (it == 0) 255 else it }, 255, 255, 255)
        drawString(op.text, op.x, op.y, paint, Canvas(bmp))
    }

    companion object {
        private const val TAG = "TextureManager"
        private fun textPaint(size: Int) = Paint().apply { textSize = size.toFloat(); color = Color.WHITE; isAntiAlias = false }

        /** Returns (width, height) of the text wrapped at 1024 px, like the original `a(String, Paint)`. */
        fun measure(str: String, paint: Paint): Pair<Int, Int> {
            val lineH = paint.textSize.toInt()
            var i = 0; var w = 0; var h = 0
            while (i < str.length) {
                val n = paint.breakText(str.substring(i), true, 1024f, null)
                if (n == 0) break
                val mw = paint.measureText(str.substring(i, i + n)).toInt()
                if (mw > w) w = mw
                h += lineH
                i += n
            }
            return Pair(w, h)
        }

        /** Port of the original `a(String, int, int, Paint, Canvas)`: wraps text to the canvas width. */
        fun drawString(str: String, x: Int, y: Int, paint: Paint, canvas: Canvas) {
            val size = paint.textSize
            var baseline = (y + size) - paint.fontMetrics.descent
            var i = 0
            while (i < str.length) {
                val n = paint.breakText(str.substring(i), true, canvas.width.toFloat(), null)
                if (n == 0) return
                canvas.drawText(str.substring(i, i + n), x.toFloat(), baseline, paint)
                baseline += size
                i += n
            }
        }
    }
}
