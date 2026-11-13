// Finds the Android SDK folder and the newest NDK / CMake / platform that are actually INSTALLED,
// so the build works on Code on the Go (offline) without hard-coding versions.
// Results are published as project extra properties: tg.sdkDir, tg.ndkPath, tg.cmakeVersion, tg.compileSdk.
import java.io.File
import java.util.Properties

fun versionParts(v: String): List<Int> = v.split('.', '-').map { it.toIntOrNull() ?: 0 }
fun compareVersions(a: String, b: String): Int {
    val x = versionParts(a); val y = versionParts(b)
    for (i in 0 until maxOf(x.size, y.size)) {
        val c = (x.getOrElse(i) { 0 }).compareTo(y.getOrElse(i) { 0 })
        if (c != 0) return c
    }
    return 0
}

val sdkCandidates = mutableListOf<String?>()
val localProps = rootProject.file("local.properties")
if (localProps.exists()) {
    val p = Properties(); localProps.inputStream().use { p.load(it) }
    sdkCandidates += p.getProperty("sdk.dir")
}
sdkCandidates += System.getenv("ANDROID_HOME")
sdkCandidates += System.getenv("ANDROID_SDK_ROOT")
sdkCandidates += "/data/data/com.itsaky.androidide/files/home/android-sdk"   // Code on the Go
val sdkDir: File? = sdkCandidates.filterNotNull().map { File(it) }.firstOrNull { it.isDirectory }

fun installedNdks(): List<File> =
    sdkDir?.resolve("ndk")?.listFiles()?.filter { it.resolve("source.properties").exists() }
        ?.sortedWith { a, b -> compareVersions(a.name, b.name) } ?: emptyList()

fun installedCmakes(): List<String> =
    sdkDir?.resolve("cmake")?.listFiles()?.filter { it.resolve("bin").isDirectory }?.map { it.name }
        ?.sortedWith { a, b -> compareVersions(a, b) } ?: emptyList()

fun installedPlatforms(): List<Int> =
    sdkDir?.resolve("platforms")?.listFiles()
        ?.mapNotNull { Regex("android-(\\d+)").matchEntire(it.name)?.groupValues?.get(1)?.toIntOrNull() }
        ?.sorted() ?: emptyList()

val wantedNdk = (findProperty("ndkVersion") as String?)?.trim().orEmpty()
val ndks = installedNdks()
val ndk: File? = ndks.firstOrNull { it.name == wantedNdk } ?: ndks.lastOrNull()
val wantedCmake = (findProperty("cmakeVersion") as String?)?.trim().orEmpty()
val cmakes = installedCmakes().filter { compareVersions(it, "3.22.1") >= 0 }
val cmake: String? = cmakes.firstOrNull { it == wantedCmake } ?: cmakes.lastOrNull()
// AGP requires android.ndkVersion to equal the NDK's real revision (from source.properties).
val ndkRevision: String? = ndk?.let { dir ->
    dir.resolve("source.properties").readLines().firstOrNull { it.startsWith("Pkg.Revision") }
        ?.substringAfter("=")?.trim()?.takeIf { it.isNotEmpty() } ?: dir.name
}
val platforms = installedPlatforms()
val wantedSdk = (findProperty("compileSdkVer") as String?)?.trim()?.toIntOrNull() ?: 35
val compileSdk: Int? = if (wantedSdk in platforms) wantedSdk else platforms.filter { it >= 33 }.maxOrNull()

println("[Tamagotchi] SDK folder : ${sdkDir ?: "NOT FOUND"}")
println("[Tamagotchi] NDK        : ${ndkRevision ?: "NONE"}   (installed: ${ndks.map { it.name }})")
println("[Tamagotchi] CMake      : ${cmake ?: "NONE >= 3.22.1"}   (installed: ${installedCmakes()})")
println("[Tamagotchi] Platform   : ${compileSdk ?: "NONE >= 33"}   (installed: $platforms)")

extra["tg.sdkDir"] = sdkDir?.absolutePath
extra["tg.ndkPath"] = ndk?.absolutePath
extra["tg.cmakeVersion"] = cmake
extra["tg.ndkVersion"] = ndkRevision
extra["tg.compileSdk"] = compileSdk

val problems = mutableListOf<String>()
if (sdkDir == null) problems += "Android SDK folder not found (set sdk.dir in local.properties)."
else {
    if (ndk == null) problems += "No NDK installed in $sdkDir/ndk. Install one (version 27 or newer) with the SDK Manager."
    if (cmakes.isEmpty()) problems += "No CMake 3.22.1 or newer in $sdkDir/cmake. Install 'cmake;3.22.1' with the SDK Manager."
    if (compileSdk == null) problems += "No Android platform 33 or newer in $sdkDir/platforms."
}
if (problems.isNotEmpty()) {
    throw GradleException("Missing SDK components for the native build:\n  - " + problems.joinToString("\n  - "))
}
