pluginManagement {
    repositories {
        google() // 이걸 명시적으로 맨 위에 둡니다.
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "CrowdMap"
include(":app")