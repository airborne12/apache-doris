/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.
 */

plugins {
    java
    idea
}

val dorisHome: String = rootProject.projectDir.parentFile.absolutePath
val dorisThirdparty: String = System.getenv("DORIS_THIRDPARTY") ?: "$dorisHome/thirdparty"

// ============ Complete Version Catalog ============
val versions = mapOf(
    // Core
    "guava" to "33.2.1-jre",
    "gson" to "2.10.1",
    "jackson" to "2.16.0",
    "protobuf" to "3.24.3",
    "thrift" to "0.16.0",

    // gRPC
    "grpc" to "1.63.0",
    "grpcJava" to "1.34.0",

    // Netty
    "netty" to "4.1.110.Final",

    // Logging
    "log4j" to "2.18.0",
    "slf4j" to "2.0.6",

    // Spring
    "spring" to "2.7.18",
    "springFramework" to "5.3.39",

    // Apache Commons
    "commonsIo" to "2.18.0",
    "commonsLang3" to "3.17.0",
    "commonsLang" to "2.6",
    "commonsCodec" to "1.13",
    "commonsCli" to "1.4",
    "commonsPool" to "1.5.1",
    "commonsPool2" to "2.2",
    "commonsText" to "1.10.0",
    "commonsValidator" to "1.9.0",
    "commonsBeanutils" to "1.11.0",
    "commonsCollections" to "3.2.2",
    "commonsConfiguration2" to "2.11.0",
    "commonsCompress" to "1.27.1",

    // Hadoop ecosystem
    "hadoop" to "3.3.6",
    "hive" to "3.1.3",
    "hbase" to "2.4.9",
    "zookeeper" to "3.9.3",

    // Data formats
    "avro" to "1.12.0",
    "parquet" to "1.15.2",
    "orc" to "1.8.4",
    "iceberg" to "1.9.1",
    "arrow" to "17.0.0",
    "paimon" to "1.1.1",

    // Parsers
    "antlr4" to "4.13.1",
    "jflex" to "1.4.3",
    "javaCup" to "0.11-a-czt02-cdh",

    // AspectJ
    "aspectj" to "1.9.7",

    // Build tools
    "lombok" to "1.18.24",

    // HTTP
    "httpclient" to "4.5.13",
    "httpcore" to "4.4.15",

    // AWS
    "awsJavaSdk" to "1.12.669",

    // Jetty
    "jetty" to "9.4.57.v20241219",

    // Kubernetes
    "fabric8" to "6.7.2",

    // Database
    "hikaricp" to "6.0.0",

    // Metrics
    "metricsCore" to "4.0.2",

    // Test
    "junit" to "5.8.2",
    "mockito" to "4.11.0",
    "jmockit" to "1.49",
    "hamcrest" to "2.1",

    // Misc
    "snappy" to "1.1.10.5",
    "kryo" to "4.0.2",
    "objenesis" to "2.1",
    "javassist" to "3.18.2-GA",
    "cglib" to "2.2",
    "re2j" to "1.8",
    "snakeyaml" to "2.0",
    "trino" to "435",

    // Doris specific
    "je" to "18.3.14-doris-SNAPSHOT",
    "hiveCatalogShade" to "3.0.1"
)

extra["versions"] = versions
extra["dorisHome"] = dorisHome
extra["dorisThirdparty"] = dorisThirdparty

allprojects {
    group = "org.apache.doris"
    version = "1.2-SNAPSHOT"

    repositories {
        mavenLocal()
        mavenCentral()
        maven { url = uri("https://repository.apache.org/content/repositories/snapshots/") }
        maven { url = uri("https://repository.cloudera.com/repository/libs-release-local/") }
        // Local libs for je, hive-catalog-shade
        flatDir {
            dirs("$dorisThirdparty/installed/lib")
        }
    }
}

subprojects {
    apply(plugin = "java")
    apply(plugin = "idea")

    java {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    tasks.withType<JavaCompile> {
        options.encoding = "UTF-8"
        options.isIncremental = true
        options.isFork = true
        options.forkOptions.memoryMaximumSize = "2g"
        options.compilerArgs.addAll(listOf("-parameters", "-Xlint:-options"))
    }

    tasks.withType<Test> {
        useJUnitPlatform()
        maxParallelForks = (Runtime.getRuntime().availableProcessors() / 2).coerceAtLeast(1)
    }

    configurations.all {
        resolutionStrategy {
            cacheChangingModulesFor(24, TimeUnit.HOURS)
            cacheDynamicVersionsFor(24, TimeUnit.HOURS)
            // Force specific versions to avoid conflicts
            force("com.google.guava:guava:${versions["guava"]}")
            force("com.google.code.gson:gson:${versions["gson"]}")
            force("io.netty:netty-all:${versions["netty"]}")
        }
    }
}

// Task to show optimization tips
tasks.register("buildInfo") {
    doLast {
        println("""
            |Doris FE Gradle Build
            |=====================
            |DORIS_HOME: $dorisHome
            |DORIS_THIRDPARTY: $dorisThirdparty
            |
            |Quick commands:
            |  ./gradlew :fe-core:compileJava    # Compile fe-core
            |  ./gradlew :fe-core:jar            # Build jar
            |  ./gradlew clean                   # Clean all
            |
            |Incremental build tips:
            |  First build may take 2-3 minutes
            |  Subsequent builds: 3-10 seconds
        """.trimMargin())
    }
}
