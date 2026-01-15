/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.
 */

plugins {
    java
}

@Suppress("UNCHECKED_CAST")
val versions = rootProject.extra["versions"] as Map<String, String>

dependencies {
    // ============ Core Dependencies ============
    implementation("com.google.guava:guava:${versions["guava"]}")
    implementation("com.google.code.gson:gson:${versions["gson"]}")
    implementation("org.apache.thrift:libthrift:${versions["thrift"]}")

    // ============ Commons ============
    implementation("commons-io:commons-io:${versions["commonsIo"]}")
    implementation("org.apache.commons:commons-lang3:${versions["commonsLang3"]}")
    implementation("org.apache.commons:commons-math3:3.1.1")

    // ============ HTTP ============
    implementation("org.apache.httpcomponents:httpclient:${versions["httpclient"]}")
    implementation("org.apache.httpcomponents:httpcore:${versions["httpcore"]}")

    // ============ Bitmap ============
    implementation("org.roaringbitmap:RoaringBitmap:0.8.13")

    // ============ Time ============
    implementation("joda-time:joda-time:2.8.1")

    // ============ AspectJ ============
    implementation("org.aspectj:aspectjweaver:${versions["aspectj"]}")
    implementation("org.aspectj:aspectjrt:${versions["aspectj"]}")

    // ============ Logging ============
    implementation("org.apache.logging.log4j:log4j-web:${versions["log4j"]}")
    implementation("org.apache.logging.log4j:log4j-iostreams:${versions["log4j"]}")
    implementation("org.apache.logging.log4j:log4j-core:${versions["log4j"]}")
    implementation("org.apache.logging.log4j:log4j-api:${versions["log4j"]}")

    // ============ Jetty ============
    implementation("org.eclipse.jetty:jetty-server:${versions["jetty"]}")
    implementation("org.eclipse.jetty:jetty-http:${versions["jetty"]}")
    implementation("org.eclipse.jetty:jetty-io:${versions["jetty"]}")
    implementation("org.eclipse.jetty:jetty-util:${versions["jetty"]}")

    // ============ Servlet ============
    implementation("javax.servlet:javax.servlet-api:3.1.0")
    implementation("javax.annotation:javax.annotation-api:1.3.2")

    // ============ Hadoop (provided) ============
    compileOnly("org.apache.hadoop:hadoop-common:${versions["hadoop"]}") {
        exclude(group = "commons-collections", module = "commons-collections")
        exclude(group = "org.apache.commons", module = "commons-compress")
        exclude(group = "org.slf4j", module = "slf4j-reload4j")
    }
    compileOnly("org.apache.hadoop:hadoop-aws:${versions["hadoop"]}")
    compileOnly("org.apache.hadoop:hadoop-aliyun:${versions["hadoop"]}")

    // ============ Cloud Storage ============
    implementation("com.amazonaws:aws-java-sdk-s3:${versions["awsJavaSdk"]}")
    implementation("com.amazonaws:aws-java-sdk-sts:${versions["awsJavaSdk"]}")
    implementation("com.aliyun.oss:aliyun-sdk-oss:3.15.0")

    // ============ Serialization ============
    implementation("com.esotericsoftware:kryo-shaded:${versions["kryo"]}")

    // ============ Trino Connector ============
    implementation("io.trino:trino-main:${versions["trino"]}") {
        exclude(group = "org.antlr", module = "antlr4-runtime")
        exclude(group = "io.airlift", module = "log")
        exclude(group = "io.airlift", module = "log-manager")
    }
    implementation("io.trino:trino-spi:${versions["trino"]}")

    // ============ Jakarta Annotations ============
    implementation("jakarta.annotation:jakarta.annotation-api:2.1.1")

    // ============ Lombok ============
    compileOnly("org.projectlombok:lombok:${versions["lombok"]}")
    annotationProcessor("org.projectlombok:lombok:${versions["lombok"]}")

    // ============ Doris Local Dependencies ============
    // hive-catalog-shade is installed in local maven repo
    compileOnly("org.apache.doris:hive-catalog-shade:${versions["hiveCatalogShade"]}")

    // ============ Test Dependencies ============
    testImplementation("org.hamcrest:hamcrest:${versions["hamcrest"]}")
    testImplementation("commons-collections:commons-collections:${versions["commonsCollections"]}")
    testImplementation("org.junit.jupiter:junit-jupiter:${versions["junit"]}")
}

tasks.jar {
    archiveBaseName.set("doris-fe-common")
}
