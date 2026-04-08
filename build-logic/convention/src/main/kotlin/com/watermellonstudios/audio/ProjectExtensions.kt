package com.watermellonstudios.audio

import org.gradle.api.Project
import org.gradle.api.artifacts.VersionCatalog
import org.gradle.api.artifacts.VersionCatalogsExtension
import org.gradle.kotlin.dsl.getByType
import java.util.Optional

val Project.libs: VersionCatalog
    get() = extensions.getByType<VersionCatalogsExtension>().named("libs")

fun VersionCatalog.version(alias: String): String =
    findVersion(alias).map { it.toString() }.orElseThrow {
        NoSuchElementException("Version catalog entry '$alias' not found")
    }

fun VersionCatalog.library(alias: String) =
    findLibrary(alias).orElseThrow {
        NoSuchElementException("Library catalog entry '$alias' not found")
    }.get()
