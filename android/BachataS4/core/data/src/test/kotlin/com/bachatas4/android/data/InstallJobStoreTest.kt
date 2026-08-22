package com.bachatas4.android.data

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class InstallJobStoreTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun createUpdateListDelete() {
        val store = InstallJobStore(temporaryFolder.root)
        val job = InstallJob(
            jobId = "j1",
            state = InstallJob.STATE_SELECTED,
            mode = ImportManager.MODE_PKG,
            sourceUri = "content://x",
            createdAtMs = 1L,
            updatedAtMs = 1L,
        )
        store.create(job)
        assertEquals(InstallJob.STATE_SELECTED, store.read("j1")!!.state)
        store.update(job.copy(state = InstallJob.STATE_EXTRACTING, updatedAtMs = 2L))
        assertEquals(InstallJob.STATE_EXTRACTING, store.read("j1")!!.state)
        assertEquals(1, store.list().size)
        assertTrue(java.io.File(temporaryFolder.root, "games/.jobs/j1/job.json").isFile)
        store.delete("j1")
        assertNull(store.read("j1"))
    }
}
