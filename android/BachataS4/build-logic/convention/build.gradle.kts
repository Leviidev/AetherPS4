plugins {
    `kotlin-dsl`
}

gradlePlugin {
    plugins {
        register("androidApplication") {
            id = "android.application.flux"
            implementationClass = "com.flux.AndroidApplicationConventionPlugin"
        }
    }
}
