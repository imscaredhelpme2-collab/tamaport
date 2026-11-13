package com.tamaport.classic

/** Thin Kotlin face of the C++ emulator core (see src/main/cpp). All calls must happen on the GL thread. */
object NativeEngine {
    init { System.loadLibrary("tgemu") }

    /** Loads the original ARM32 engine library. Returns an error message, or null on success. */
    @JvmStatic external fun nativeCreate(host: GameHost, engineLib: ByteArray): String?
    @JvmStatic external fun nativeInitGl(): Boolean
    /** width/height = size in pixels of the game viewport. Returns an error message, or null. */
    @JvmStatic external fun nativeStart(width: Int, height: Int): String?
    @JvmStatic external fun nativeFrame(surfaceW: Int, surfaceH: Int, vx: Int, vy: Int, vw: Int, vh: Int)
    @JvmStatic external fun nativeTouch(id: Int, x: Float, y: Float, hx: Float, hy: Float)
    @JvmStatic external fun nativeRelease(id: Int, x: Float, y: Float)
    @JvmStatic external fun nativeGetPart(): Int
    @JvmStatic external fun nativeSetAppRequest(v: Int)
    @JvmStatic external fun nativeMakeAlarmInfo()
    @JvmStatic external fun nativeStop()
    @JvmStatic external fun nativeTerm()
    @JvmStatic external fun nativeFailure(): String?
    /** 8-byte header (w, h as big-endian ints) followed by RGBA pixels, top row first. */
    @JvmStatic external fun nativeReadPixels(): ByteArray?
    @JvmStatic external fun nativeDestroy()

    const val BASE_W = 320
    const val BASE_H = 512
}
