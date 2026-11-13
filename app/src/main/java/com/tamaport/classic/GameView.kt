package com.tamaport.classic

import android.opengl.GLSurfaceView
import android.view.MotionEvent
import javax.microedition.khronos.egl.EGLConfig
import javax.microedition.khronos.opengles.GL10

/**
 * Renders the emulated engine. The 320x512 game is letterboxed into any screen shape; touches are converted back
 * into the engine's coordinate space. All engine access is on the GL thread.
 */
class GameView(private val activity: GameActivity, private val host: GameHost) : GLSurfaceView(activity) {
    private var surfaceW = 0; private var surfaceH = 0
    private var vx = 0; private var vy = 0; private var vw = 0; private var vh = 0
    private var initVw = 0                        // viewport width the engine was started with
    private var started = false
    private var contextCreatedBefore = false
    private var failureShown = false

    // After returning from the background the original shell skipped 10 frames, then told the engine to resync.
    @Volatile private var resumePending = false
    private var resumeFrames = 0
    @Volatile private var captureRequested = false

    private val renderer = object : Renderer {
        override fun onSurfaceCreated(gl: GL10?, config: EGLConfig?) {
            if (!NativeEngine.nativeInitGl()) { activity.fatal("OpenGL initialisation failed"); return }
            if (contextCreatedBefore) host.textures.onContextLost()
            contextCreatedBefore = true
        }

        override fun onSurfaceChanged(gl: GL10?, width: Int, height: Int) {
            surfaceW = width; surfaceH = height
            val scale = minOf(width / NativeEngine.BASE_W.toFloat(), height / NativeEngine.BASE_H.toFloat())
            vw = (NativeEngine.BASE_W * scale).toInt(); vh = (NativeEngine.BASE_H * scale).toInt()
            vx = (width - vw) / 2; vy = (height - vh) / 2
            activity.setGameViewport(vx, height - vy - vh, vw, vh)   // top-left origin for Android views
            if (!started) {
                val err = NativeEngine.nativeStart(vw, vh)
                if (err != null) { activity.fatal(err); return }
                started = true; initVw = vw
                host.sound.init()
            }
        }

        override fun onDrawFrame(gl: GL10?) {
            if (!started) return
            if (resumePending) {
                if (resumeFrames == 0) host.sound.resumeAll()
                if (resumeFrames < 10) { resumeFrames++; return }
                resumePending = false
                NativeEngine.nativeSetAppRequest(2)
            }
            NativeEngine.nativeFrame(surfaceW, surfaceH, vx, vy, vw, vh)
            if (captureRequested) { captureRequested = false; activity.onCaptured(NativeEngine.nativeReadPixels()) }
            if (!failureShown) {
                val f = NativeEngine.nativeFailure()
                if (f != null) { failureShown = true; activity.fatal("The game engine stopped: $f") }
            }
        }
    }

    init {
        setEGLContextClientVersion(2)
        setEGLConfigChooser(8, 8, 8, 8, 0, 8)      // the engine uses the stencil buffer for clipping
        setRenderer(renderer)
        renderMode = RENDERMODE_CONTINUOUSLY
    }

    fun markResume() { resumePending = true; resumeFrames = 0 }
    fun requestCapture() { captureRequested = true }

    private fun toEngineX(x: Float) = (x - vx) * initVw / vw.coerceAtLeast(1)
    private fun toEngineY(y: Float) = (y - vy) * initVw / vw.coerceAtLeast(1)

    override fun onTouchEvent(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> {
                val x = e.x; val y = e.y
                val hx = if (e.historySize > 0) e.getHistoricalX(0) else x
                val hy = if (e.historySize > 0) e.getHistoricalY(0) else y
                queueEvent { if (started) NativeEngine.nativeTouch(0, toEngineX(x), toEngineY(y), toEngineX(hx), toEngineY(hy)) }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                val x = e.x; val y = e.y
                queueEvent { if (started) NativeEngine.nativeRelease(-1, toEngineX(x), toEngineY(y)) }
            }
            else -> return super.onTouchEvent(e)
        }
        return true
    }
}
