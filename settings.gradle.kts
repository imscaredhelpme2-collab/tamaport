pluginManagement {
    // Plugin versions come from gradle.properties, so a different toolchain only needs that one file changed.
    val agpVersion: String by settings
    val kotlinVersion: String by settings
    plugins {
        id("com.android.application") version agpVersion
        id("org.jetbrains.kotlin.android") version kotlinVersion
    }
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
    }
}
rootProject.name = "TamagotchiClassic"
include(":app")
