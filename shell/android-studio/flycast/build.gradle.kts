// hotd2-vr: modified in 2026 by mikermak for the Quest VR mode (see "git log master..hotd2-vr").
import java.util.Properties

plugins {
    alias(libs.plugins.android.application)
}

fun gitVersionName(): String {
    return providers.exec {
        commandLine("git", "describe", "--tags", "--always")
    }.standardOutput.asText.get().trim()
}

// hotd2-vr: Ninja Assault VR's own version. The vr build's versionName is "<hubVersion>-g<commit>"
// (set in androidComponents below); the standalone release starts at versionCode 1.
val hubVersion = "1.0.7"

fun gitShortHash(): String {
    return providers.exec {
        commandLine("git", "rev-parse", "--short", "HEAD")
    }.standardOutput.asText.get().trim()
}

// hotd2-vr: VR Light Gun Hub's release key, kept outside the repository (the committed
// debug.keystore is public, so anyone could sign an "update" with it). A properties file with
// storeFile (relative to the properties file), storePassword, keyAlias and keyPassword: the path
// in the environment variable VRLGH_SIGNING, else ~/.vrlightgunhub/release-signing.properties.
// hotd2-vr/make-release-key.ps1 makes the key and the file. Without it: the debug key, and a warning.
val hubSigningFile: File = providers.environmentVariable("VRLGH_SIGNING").orNull
    ?.takeIf { it.isNotBlank() }?.let { File(it) }
    ?: File(System.getProperty("user.home"), ".vrlightgunhub/release-signing.properties")
val hubSigning: Properties? = if (hubSigningFile.isFile)
    Properties().apply { hubSigningFile.inputStream().use { load(it) } } else null

fun hubSigningValue(key: String): String =
    hubSigning?.getProperty(key)?.trim()?.takeIf { it.isNotEmpty() }
        ?: throw GradleException("hotd2-vr: $hubSigningFile has no $key")

android {
    namespace = "com.flycast.emulator"
    ndkVersion = "29.0.14206865"
    compileSdk {
        version = release(36) {
            minorApiLevel = 1
        }
    }

    defaultConfig {
        applicationId = "com.flycast.emulator"
        minSdk = 29     // hotd2-vr: Quest only (the OpenXR loader needs 24+)
        targetSdk = 36
        versionCode = 8
        versionName = gitVersionName()
        vectorDrawables.useSupportLibrary = true

        externalNativeBuild {
            cmake {
                arguments += "-DANDROID_ARM_MODE=arm"
                arguments += "-DSENTRY_UPLOAD_URL=" + (System.getenv("SENTRY_UPLOAD_URL") ?: "")
                arguments += "-DUSE_OPENMP=OFF"
                arguments += "-DANDROID_WEAK_API_DEFS=ON"
            }
        }

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }

    signingConfigs {
        getByName("debug") {
            storeFile = file("../debug.keystore")
        }
        create("release") {
            storeFile = file("../playstore.jks")
            storePassword = System.getenv("ANDROID_KEYSTORE_PASSWORD")
            keyAlias = "uploadkey"
            keyPassword = System.getenv("ANDROID_KEYSTORE_PASSWORD")
        }
        // hotd2-vr: VR Light Gun Hub's release key, when its properties file is there (above)
        if (hubSigning != null) {
            create("hub") {
                storeFile = hubSigningFile.absoluteFile.parentFile.resolve(hubSigningValue("storeFile"))
                storePassword = hubSigningValue("storePassword")
                keyAlias = hubSigningValue("keyAlias")
                keyPassword = hubSigningValue("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            signingConfig = signingConfigs.getByName("release")
        }
        // hotd2-vr: optimised build for sideloading on the Quest, signed with the app's release
        // key ("hub" above), or the public debug key when there is none (a test build only).
        // The app Ninja Assault VR: its applicationId and versionName are set below
        // (androidComponents), so it installs next to (never over) a regular Flycast.
        create("vr") {
            initWith(getByName("release"))
            val hub = signingConfigs.findByName("hub")
            if (hub == null) {
                logger.warn("hotd2-vr: no release key, $hubSigningFile not found (set VRLGH_SIGNING, or run " +
                    "hotd2-vr\\make-release-key.ps1): the vr build is signed with the public debug key, not for release")
            }
            signingConfig = hub ?: signingConfigs.getByName("debug")
            // run-as for copying games into app storage and reading logs
            isDebuggable = true
            // ...but a debuggable variant builds native code as Debug (-O0: ~20 fps).
            // The last CMAKE_BUILD_TYPE on the command line wins.
            externalNativeBuild {
                cmake {
                    arguments += "-DCMAKE_BUILD_TYPE=Release"
                    arguments += "-DUSE_OPENXR=ON"
                    arguments += "-DVR_STANDALONE_LUPIN=OFF"
                    arguments += "-DVR_STANDALONE_NINJA=ON"
                }
            }
            // The Quest is arm64. (Not with -Pandroid.injected.build.abi: that's the IDE's
            // build for one device, and it marks the APK test-only, which SideQuest and a
            // plain adb install refuse.)
            ndk {
                abiFilters += "arm64-v8a"
            }
            versionNameSuffix = "-vr"
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }
    externalNativeBuild {
        cmake {
            path = file("../../../CMakeLists.txt")
            version = "3.22.1"
        }
    }
    buildFeatures {
        prefab = true   // hotd2-vr: OpenXR loader headers and library for CMake
    }
    packaging {
        jniLibs {
            // not used
            excludes += "lib/*/libz.so"
            // This is necessary for libadrenotools custom driver loading
            useLegacyPackaging = true
        }
        resources {
            excludes += "META-INF/DEPENDENCIES"
        }
    }
}

// hotd2-vr: the models that come with the app, hotd2-vr/models (guns/, controllers/, CREDITS.txt): filled
// by hand from the models' own downloads for a release build and never in git (.gitignore; the README's
// Releasing). The vr build's assets/models when that folder is there, else none (the plain public build).
// BaseGLActivity copies them out to the data folder at start.
abstract class BundledModels : DefaultTask() {
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val models: DirectoryProperty

    @get:OutputDirectory
    abstract val assets: DirectoryProperty

    @TaskAction
    fun copy() {
        val from = models.get().asFile
        val out = assets.get().asFile
        out.deleteRecursively()
        // the models and their sidecars, and the credits: nothing else that may lie there
        from.walkTopDown().filter {
            it.isFile && (it.extension.lowercase() in setOf("glb", "json") || it.name == "CREDITS.txt")
        }.forEach { it.copyTo(File(out, "models/" + it.relativeTo(from).invariantSeparatorsPath), overwrite = true) }
    }
}
val bundledModels = file("../../../hotd2-vr/models")

// hotd2-vr: the licence the app goes out under, assets/LICENSE.txt in the vr build (the APK is downloaded on its own):
// GPL-3.0 (the copy that comes with Flycast's picoTCP), cgltf's MIT notice (core/deps/cgltf, it reads the GLB models)
// and, with the bundled models, a line that they aren't under it. package-release.ps1 copies this file out of the
// APK as the PC zip's LICENSE.txt, so the two are the same text, and checks both for the GPL and cgltf's notice.
abstract class LicenceText : DefaultTask() {
    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val gpl3: RegularFileProperty

    @get:InputFile
    @get:PathSensitive(PathSensitivity.NONE)
    abstract val cgltf: RegularFileProperty

    @get:Input
    abstract val withModels: Property<Boolean>

    @get:OutputDirectory
    abstract val assets: DirectoryProperty

    @TaskAction
    fun write() {
        val gpl = gpl3.get().asFile.readText()
        if (!Regex("GNU GENERAL PUBLIC LICENSE\\s+Version 3, 29 June 2007").containsMatchIn(gpl))
            throw GradleException("hotd2-vr: no GPL-3.0 text in ${gpl3.get().asFile}")
        val mit = cgltf.get().asFile.readText().trim()
        if (!Regex("^Copyright \\(c\\) [0-9-]+ Johannes Kuhlmann\\s+Permission is hereby granted").containsMatchIn(mit))
            throw GradleException("hotd2-vr: no MIT notice in ${cgltf.get().asFile}")
        val head = listOf(
            "Ninja Assault VR: licence",
            "",
            "Ninja Assault VR (the Quest app, and flycast.exe in the PC version) is built from",
            "Flycast (GPL-2.0-or-later), mikermak's hotd2-vr and Confidential Mission VR's changes to it",
            "(GPL-2.0-or-later too), and the gun model \"Namco Arcade Gun\" by Martoscar (CC BY 4.0,",
            "https://creativecommons.org/licenses/by/4.0/). CC BY 4.0 goes together with GPL-3.0 but not",
            "GPL-2.0, so the app is distributed under the GNU General Public License version 3, below. Its",
            "source code is published with every release, on the same page as the APK and the zip.",
            "",
            "It also contains cgltf by Johannes Kuhlmann (https://github.com/jkuhlmann/cgltf), which reads",
            "the 3D models of the guns and the controllers, under the MIT licence:",
            "", "")
        val models = if (!withModels.get()) emptyList() else listOf(
            "The 3D models that come with the app (assets/models in the Quest app, data\\models in the PC",
            "version) are not part of the program and not under its licence: each is its author's, under",
            "its own licence, as CREDITS.txt says.",
            "", "")
        val text = head.joinToString("\n") + mit + "\n\n" + models.joinToString("\n") + gpl
        val out = assets.get().asFile
        out.deleteRecursively()
        out.mkdirs()
        // CRLF: the zip's copy is read on Windows
        File(out, "LICENSE.txt").writeText(text.replace("\r\n", "\n").replace("\n", "\r\n"), Charsets.US_ASCII)
    }
}

// hotd2-vr: the vr build type is the app Ninja Assault VR, with an application ID of its own
// (BaseGLActivity.VR_APPLICATION_ID matches it). ${applicationId} in the manifest follows.
// Its versionName and versionCode are the standalone app's own, not Flycast's tags.
androidComponents {
    onVariants(selector().withBuildType("vr")) { variant ->
        variant.applicationId.set("com.vrlightgunhub.ninjaassault")
        variant.outputs.forEach {
            it.versionName.set("$hubVersion-g${gitShortHash()}")
            it.versionCode.set(7)
        }
        val licence = tasks.register<LicenceText>("${variant.name}LicenceText") {
            gpl3.set(file("../../../core/deps/picotcp/LICENSE.GPLv3"))
            cgltf.set(file("../../../core/deps/cgltf/LICENSE"))
            withModels.set(bundledModels.isDirectory)
        }
        variant.sources.assets?.addGeneratedSourceDirectory(licence, LicenceText::assets)
        if (bundledModels.isDirectory) {
            val copy = tasks.register<BundledModels>("${variant.name}BundledModels") {
                models.set(bundledModels)
            }
            variant.sources.assets?.addGeneratedSourceDirectory(copy, BundledModels::assets)
            logger.lifecycle("hotd2-vr: hotd2-vr/models goes in the ${variant.name} build as assets/models")
        }
    }
}

dependencies {
    implementation(libs.appcompat)
    implementation(libs.commons.lang3)
    implementation(libs.httpclient5)
    implementation(libs.slf4j.android)
    implementation(fileTree("libs") { include("*.aar", "*.jar") })
    implementation(libs.documentfile)
    implementation("org.khronos.openxr:openxr_loader_for_android:1.1.43")
    testImplementation(libs.junit)
    androidTestImplementation(libs.espresso.core)
    androidTestImplementation(libs.ext.junit)
}