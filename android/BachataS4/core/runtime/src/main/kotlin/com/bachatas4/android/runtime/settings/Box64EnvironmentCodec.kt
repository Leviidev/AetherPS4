package com.bachatas4.android.runtime.settings

object Box64EnvironmentCodec {
    private val namePattern = Regex("BOX64_[A-Z0-9_]+")
    private val launchOwned = setOf(
        "BOX64_PATH",
        "BOX64_LD_LIBRARY_PATH",
        "BOX64_EMULATED_LIBS",
        "BOX64_LOAD_ADDR",
        "BOX64_PROFILE",
    )

    fun decode(text: String): Map<String, String> {
        val values = linkedMapOf<String, String>()
        text.lineSequence().forEachIndexed { index, rawLine ->
            val line = rawLine.trim()
            if (line.isBlank() || line.startsWith('#')) return@forEachIndexed
            val separator = line.indexOf('=')
            require(separator > 0) { "Invalid Box64 entry on line ${index + 1}" }
            val name = line.substring(0, separator).trim()
            val value = line.substring(separator + 1)
            validate(name, value)
            require(values.put(name, value) == null) { "Duplicate Box64 entry $name" }
        }
        return values
    }

    fun encode(values: Map<String, String>): String = buildString {
        values.toSortedMap().forEach { (name, value) ->
            validate(name, value)
            append(name).append('=').append(value).append('\n')
        }
    }

    private fun validate(name: String, value: String) {
        require(namePattern.matches(name)) { "Invalid Box64 variable $name" }
        require(name !in launchOwned) { "$name is launch-owned by BachataS4" }
        require('\u0000' !in value && '\n' !in value && '\r' !in value) { "Invalid value for $name" }
    }
}
