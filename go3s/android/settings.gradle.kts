import java.util.Properties

pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
val sdkCredentials = Properties().apply {
    val file = file("sdk-credentials.properties")
    if (file.exists()) file.inputStream().use { load(it) }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
        val sdkUrl = sdkCredentials.getProperty("sdk.repository.url")
        if (!sdkUrl.isNullOrBlank()) maven {
            url = uri(sdkUrl)
            credentials {
                username = sdkCredentials.getProperty("sdk.repository.username")
                password = sdkCredentials.getProperty("sdk.repository.password")
            }
            content {
                includeGroupByRegex("com\\.arashivision.*")
                includeGroupByRegex("com\\.insta360.*")
            }
        }
    }
}
rootProject.name = "Go3sBridge"
include(":app")
