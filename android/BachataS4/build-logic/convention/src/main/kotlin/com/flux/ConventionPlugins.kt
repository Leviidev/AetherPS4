package com.flux

import org.gradle.api.Plugin
import org.gradle.api.Project

class AndroidApplicationConventionPlugin : Plugin<Project> {
    override fun apply(target: Project) {
        // Intentionally minimal in v1. Real convention logic is out of scope.
    }
}
