package com.bachatas4.android.data

import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class SampleRepository @Inject constructor() {
    fun greeting(): String = "Hello from BachataS4"
}
