plugins {
    id("com.android.application")
    kotlin("android")
}

// Release signing is deliberately opt-in. The keystore must live outside the
// checkout and all four values must be present before AGP is given a signing
// configuration. Otherwise release remains unsigned.
val externalKeystoreFile = System.getenv("PDB_ANDROID_KEYSTORE_FILE")?.trim().orEmpty()
val externalKeystorePassword = System.getenv("PDB_ANDROID_KEYSTORE_PASSWORD")?.trim().orEmpty()
val externalKeyAlias = System.getenv("PDB_ANDROID_KEY_ALIAS")?.trim().orEmpty()
val externalKeyPassword = System.getenv("PDB_ANDROID_KEY_PASSWORD")?.trim().orEmpty()
val externalSigningReady = externalKeystoreFile.isNotEmpty() &&
    externalKeystorePassword.isNotEmpty() &&
    externalKeyAlias.isNotEmpty() &&
    externalKeyPassword.isNotEmpty() &&
    file(externalKeystoreFile).isFile

android {
    namespace = "io.paddrawboard.client"
    compileSdk = 36

    defaultConfig {
        applicationId = "io.paddrawboard.client"
        minSdk = 30
        targetSdk = 36
        versionCode = 1
        versionName = "0.1.0"
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
    }
    buildFeatures { buildConfig = true }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
    sourceSets {
        getByName("main").java.srcDir(file("../../protocol/kotlin"))
    }
    testOptions { unitTests.isIncludeAndroidResources = true }

    if (externalSigningReady) {
        signingConfigs {
            create("externalRelease") {
                storeFile = file(externalKeystoreFile)
                storePassword = externalKeystorePassword
                keyAlias = externalKeyAlias
                keyPassword = externalKeyPassword
            }
        }
    }
    buildTypes {
        getByName("release") {
            signingConfig = if (externalSigningReady) {
                signingConfigs.getByName("externalRelease")
            } else {
                null
            }
        }
    }
}

dependencies {
    testImplementation("junit:junit:4.13.2")
    androidTestImplementation("androidx.test.ext:junit:1.2.1")
    androidTestImplementation("androidx.test:runner:1.6.2")
}
