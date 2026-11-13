plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

apply(from = rootProject.file("gradle/sdk-detect.gradle.kts"))   // finds the installed NDK / CMake / SDK platform

val compileSdkVer = extra["tg.compileSdk"] as Int
val targetSdkVer = minOf((project.findProperty("targetSdkVer") as String).toInt(), compileSdkVer)
val abiList = (project.findProperty("abis") as String).split(",").map { it.trim() }.filter { it.isNotEmpty() }

android {
    namespace = "com.tamaport.classic"
    compileSdk = compileSdkVer
    ndkVersion = extra["tg.ndkVersion"] as String   // must equal the revision of the NDK at ndkPath
    ndkPath = extra["tg.ndkPath"] as String

    defaultConfig {
        applicationId = "com.tamaport.classic"
        minSdk = 26
        targetSdk = targetSdkVer
        versionCode = 1
        versionName = "1.0.2-modern"

        ndk { abiFilters += abiList }
        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
                arguments += listOf("-DANDROID_STL=c++_static")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = extra["tg.cmakeVersion"] as String
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // Signed with the debug key so the release APK installs directly (personal use).
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}
