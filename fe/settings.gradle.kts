/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.
 */

rootProject.name = "doris-fe"

include("fe-common")
include("fe-core")

// Enable build cache for faster incremental builds
buildCache {
    local {
        isEnabled = true
        directory = File(rootDir, ".gradle/build-cache")
    }
}

// Plugin management
pluginManagement {
    repositories {
        gradlePluginPortal()
        mavenCentral()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.PREFER_PROJECT)
    repositories {
        mavenCentral()
        maven { url = uri("https://repository.apache.org/content/repositories/snapshots/") }
        maven { url = uri("https://repository.cloudera.com/repository/libs-release-local/") }
    }
}
