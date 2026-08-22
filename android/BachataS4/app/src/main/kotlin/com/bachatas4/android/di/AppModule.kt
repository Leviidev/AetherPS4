package com.bachatas4.android.di

import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.components.SingletonComponent
import com.bachatas4.android.BuildConfig
import com.bachatas4.android.feature.setup.DownloadRuntime

@Module
@InstallIn(SingletonComponent::class)
object AppModule {
    @Provides
    @DownloadRuntime
    fun provideDownloadRuntime(): Boolean = BuildConfig.DOWNLOAD_RUNTIME
}
