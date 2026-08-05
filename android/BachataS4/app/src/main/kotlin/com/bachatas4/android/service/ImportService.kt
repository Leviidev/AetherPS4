package com.bachatas4.android.service

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.net.Uri
import android.os.IBinder
import android.os.ParcelFileDescriptor
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.documentfile.provider.DocumentFile
import com.bachatas4.android.MainActivity
import com.bachatas4.android.data.ContentImportException
import com.bachatas4.android.data.ContentImportRequest
import com.bachatas4.android.data.ContentImporter
import com.bachatas4.android.data.ContentTreeEntry
import com.bachatas4.android.data.GameInstallVerifier
import com.bachatas4.android.data.GameMetadataResolver
import com.bachatas4.android.data.GameRepository
import com.bachatas4.android.data.ImportManager
import com.bachatas4.android.data.ImportProgress
import com.bachatas4.android.data.InstallCleanup
import com.bachatas4.android.data.InstallErrorCode
import com.bachatas4.android.data.InstallJob
import com.bachatas4.android.data.InstallJobStore
import com.bachatas4.android.data.InstallValidator
import com.bachatas4.android.data.ParamSfoReader
import com.bachatas4.android.data.PkgKeyStore
import com.bachatas4.android.model.RuntimeErrorCode
import com.bachatas4.android.runtime.pkg.PkgExtractor
import com.bachatas4.android.runtime.pkg.PkgProbeResult
import com.bachatas4.android.runtime.pkg.PkgStatus
import dagger.hilt.android.AndroidEntryPoint
import java.io.File
import java.io.FileOutputStream
import java.util.UUID
import javax.inject.Inject
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.coroutines.coroutineContext

/**
 * Imports a user-selected game folder or PS4 `.pkg` into app storage.
 *
 * Runs as a normal (non-foreground) service. Progress updates [ImportManager]
 * and optional status-bar notifications (no startForeground).
 */
@AndroidEntryPoint
class ImportService : Service() {
    @Inject lateinit var contentImporter: ContentImporter
    @Inject lateinit var gameRepository: GameRepository
    @Inject lateinit var pkgKeyStore: PkgKeyStore

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var importJob: Job? = null
    private var passcodeWaiter: CompletableDeferred<String?>? = null
    private var copyConfirmWaiter: CompletableDeferred<Boolean>? = null

    override fun onCreate() {
        super.onCreate()
        createNotificationChannel()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ImportManager.ACTION_CANCEL -> {
                Log.i(TAG, "cancel requested")
                PkgExtractor.nativeCancel()
                passcodeWaiter?.complete(null)
                copyConfirmWaiter?.complete(false)
                importJob?.cancel()
                getSystemService(NotificationManager::class.java).cancel(NOTIFICATION_ID)
                if (importJob?.isActive != true) {
                    ImportManager.reset()
                    stopSelf()
                }
                return START_NOT_STICKY
            }
            ImportManager.ACTION_SUBMIT_PASSCODE -> {
                val code = intent.getStringExtra(ImportManager.EXTRA_PASSCODE)
                Log.i(TAG, "passcode submitted (len=${code?.length ?: 0})")
                passcodeWaiter?.complete(code)
                return START_NOT_STICKY
            }
            ImportManager.ACTION_CONFIRM_PKG_COPY -> {
                Log.i(TAG, "pkg copy confirmed by user")
                copyConfirmWaiter?.complete(true)
                return START_NOT_STICKY
            }
            ImportManager.ACTION_IMPORT -> {
                val uriString = intent.getStringExtra(ImportManager.EXTRA_URI) ?: run {
                    Log.e(TAG, "import missing source URI")
                    ImportManager.update(
                        ImportProgress.Failed(InstallErrorCode.SOURCE_INACCESSIBLE, "Missing source URI"),
                    )
                    stopSelf()
                    return START_NOT_STICKY
                }
                val mode = intent.getStringExtra(ImportManager.EXTRA_MODE) ?: ImportManager.MODE_FOLDER
                Log.i(TAG, "import start mode=$mode uri=$uriString")
                if (importJob?.isActive == true) {
                    Log.w(TAG, "import already running — ignore new request")
                    return START_NOT_STICKY
                }
                if (!ImportManager.tryBeginImport(uriString, mode)) {
                    ImportManager.reset()
                    if (!ImportManager.tryBeginImport(uriString, mode)) {
                        Log.w(TAG, "import slot busy — abort")
                        return START_NOT_STICKY
                    }
                }
                importJob = scope.launch {
                    when (mode) {
                        ImportManager.MODE_PKG -> runPkgImport(uriString)
                        else -> runFolderImport(uriString)
                    }
                }
            }
        }
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        importJob?.cancel()
        passcodeWaiter?.complete(null)
        copyConfirmWaiter?.complete(false)
        if (ImportManager.isBusy()) {
            ImportManager.reset()
        }
        scope.cancel()
        super.onDestroy()
    }

    private suspend fun runFolderImport(uriString: String) {
        Log.i(TAG, "folder import prepare uri=$uriString")
        updateNotification("Validating folder…", indeterminate = true)
        val jobId = UUID.randomUUID().toString()
        val jobStore = InstallJobStore(filesDir)
        var job = InstallJob(
            jobId = jobId,
            state = InstallJob.STATE_SELECTED,
            mode = ImportManager.MODE_FOLDER,
            sourceUri = uriString,
            createdAtMs = System.currentTimeMillis(),
            updatedAtMs = System.currentTimeMillis(),
        )
        jobStore.create(job)
        try {
            ImportManager.update(ImportProgress.Validating(uriString, ImportManager.MODE_FOLDER))
            job = job.copy(state = InstallJob.STATE_VALIDATING, updatedAtMs = System.currentTimeMillis())
            jobStore.update(job)

            InstallValidator.checkWritableGamesDir(filesDir)?.let { code ->
                failInstall(code, "Games directory is not writable")
            }

            val uri = Uri.parse(uriString)
            val persistable = runCatching {
                contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                true
            }.getOrElse {
                Log.w(TAG, "folder persistable permission: ${it.message}")
                false
            }
            job = job.copy(uriPersistable = persistable, updatedAtMs = System.currentTimeMillis())
            jobStore.update(job)

            Log.i(TAG, "folder tree scan start")
            val (folderName, entries) = withContext(Dispatchers.IO) {
                val root = requireNotNull(
                    DocumentFile.fromTreeUri(this@ImportService, uri),
                ) { "Cannot read selected folder" }
                (root.name?.ifBlank { null } ?: "Imported game") to root.toImportEntries()
            }
            Log.i(TAG, "folder tree scan done name=$folderName files=${entries.size}")

            if (entries.none { it.relativePath == "eboot.bin" }) {
                failInstall(InstallErrorCode.VERIFY_FAILED, "Selected folder has no eboot.bin")
            }
            if (entries.none { it.relativePath == "sce_sys/param.sfo" }) {
                failInstall(InstallErrorCode.VERIFY_FAILED, "Selected folder has no sce_sys/param.sfo")
            }

            ImportManager.update(ImportProgress.ReadingMetadata(folderName, null))
            job = job.copy(
                state = InstallJob.STATE_READING_METADATA,
                displayName = folderName,
                updatedAtMs = System.currentTimeMillis(),
            )
            jobStore.update(job)
            updateNotification("Reading metadata…", indeterminate = true)

            val sfoEntry = entries.firstOrNull { it.relativePath == "sce_sys/param.sfo" }
            val sfoBytes = sfoEntry?.let { entry ->
                withContext(Dispatchers.IO) {
                    runCatching {
                        contentResolver.openInputStream(Uri.parse(entry.sourceUri))
                            ?.use { it.readBytes() }
                    }.getOrNull()
                }
            }
            val sfo = sfoBytes?.let { ParamSfoReader.parse(it) }
            val resolved = GameMetadataResolver.resolve(folderName = folderName, sfo = sfo)
            if (resolved.id.isBlank()) {
                failInstall(InstallErrorCode.NO_TITLE_ID, "Could not determine game title id")
            }
            Log.i(TAG, "folder copy start id=${resolved.id} title=${resolved.title} files=${entries.size}")

            val dest = File(filesDir, "games/${resolved.id}")
            if (dest.exists()) {
                if (GameInstallVerifier.canLaunch(filesDir, "games/${resolved.id}")) {
                    failInstall(InstallErrorCode.ALREADY_INSTALLED, "Game already installed")
                }
                // Incomplete leftover — remove so reinstall can proceed
                dest.deleteRecursively()
            }

            job = job.copy(
                state = InstallJob.STATE_COPYING,
                titleId = resolved.id,
                stagingDir = "games/.import-$jobId",
                updatedAtMs = System.currentTimeMillis(),
            )
            jobStore.update(job)

            val result = contentImporter.importGameTree(
                ContentImportRequest(
                    id = resolved.id,
                    title = resolved.title,
                    sourceUri = uriString,
                    subtitle = resolved.subtitle,
                    detail = resolved.detail,
                ),
                entries,
                onProgress = { bytesCopied, totalBytes, currentFile ->
                    ImportManager.update(
                        ImportProgress.Copying(
                            bytesCopied = bytesCopied,
                            totalBytes = totalBytes,
                            currentFile = currentFile,
                            gameTitle = resolved.title,
                        ),
                    )
                    val (max, progress) = scaledProgress(bytesCopied, totalBytes)
                    val sizeText = if (totalBytes > 0) {
                        "${formatBytes(bytesCopied)} / ${formatBytes(totalBytes)}"
                    } else {
                        formatBytes(bytesCopied)
                    }
                    updateNotification(
                        "Importing ${resolved.title} · $sizeText",
                        maxProgress = max,
                        progress = progress,
                        indeterminate = max == 0,
                    )
                },
            )

            ImportManager.update(ImportProgress.Verifying(resolved.title))
            ImportManager.update(ImportProgress.Registering(resolved.title))
            job = job.copy(state = InstallJob.STATE_REGISTERING, updatedAtMs = System.currentTimeMillis())
            jobStore.update(job)

            gameRepository.addImportedGame(result, uriString, System.currentTimeMillis())
            jobStore.delete(jobId)
            ImportManager.update(ImportProgress.Installed(resolved.id, resolved.title))
            Log.i(TAG, "folder import success id=${resolved.id}")
            notifyDone("${resolved.title} installed")
        } catch (failure: Throwable) {
            InstallCleanup(filesDir, jobStore).cleanupJob(jobId)
            handleFailure(failure)
        } finally {
            Log.i(TAG, "folder import finished (stopSelf)")
            stopSelf()
        }
    }

    private suspend fun runPkgImport(uriString: String) {
        Log.i(TAG, "pkg import prepare uri=$uriString")
        updateNotification("Validating package…", indeterminate = true)
        val jobId = UUID.randomUUID().toString()
        val jobStore = InstallJobStore(filesDir)
        var job = InstallJob(
            jobId = jobId,
            state = InstallJob.STATE_SELECTED,
            mode = ImportManager.MODE_PKG,
            sourceUri = uriString,
            createdAtMs = System.currentTimeMillis(),
            updatedAtMs = System.currentTimeMillis(),
        )
        jobStore.create(job)
        var staging: File? = null
        var cacheFile: File? = null
        var completed = false
        try {
            ImportManager.update(ImportProgress.Validating(uriString, ImportManager.MODE_PKG))
            job = job.copy(state = InstallJob.STATE_VALIDATING, updatedAtMs = System.currentTimeMillis())
            jobStore.update(job)

            InstallValidator.checkWritableGamesDir(filesDir)?.let { code ->
                failInstall(code, "Games directory is not writable")
            }

            val uri = Uri.parse(uriString)
            val persistable = runCatching {
                contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                true
            }.getOrElse {
                Log.w(TAG, "pkg persistable permission: ${it.message}")
                false
            }
            job = job.copy(uriPersistable = persistable, updatedAtMs = System.currentTimeMillis())
            jobStore.update(job)

            // Probe via SAF fd (header-only; sequential extract will use a local cache).
            val probe: PkgProbeResult
            try {
                withContext(Dispatchers.IO) {
                    contentResolver.openFileDescriptor(uri, "r")
                        ?: error("Cannot open PKG")
                }.use { descriptor ->
                    Log.i(TAG, "pkg openFileDescriptor ok sizeHint=${descriptor.statSize}")
                    Log.i(TAG, "pkg nativeProbe start fd=${descriptor.fd}")
                    probe = withContext(Dispatchers.IO) { PkgExtractor.nativeProbe(descriptor.fd) }
                }
            } catch (e: SecurityException) {
                failInstall(InstallErrorCode.PERMISSION_LOST, e.message ?: "Permission lost")
            } catch (e: Exception) {
                failInstall(InstallErrorCode.SOURCE_INACCESSIBLE, e.message ?: "Cannot open PKG")
            }
            Log.i(
                TAG,
                "pkg nativeProbe done status=${probe.status} contentId=${probe.contentId} " +
                    "pkgSize=${probe.packageSize} pfsSize=${probe.pfsImageSize} " +
                    "hint=${probe.titleHint} msg=${probe.message}",
            )
            val displayName = probe.titleHint?.ifBlank { null }
                ?: probe.contentId.ifBlank { "PKG" }
            ImportManager.update(ImportProgress.ReadingMetadata(displayName, probe.contentId))
            job = job.copy(
                state = InstallJob.STATE_READING_METADATA,
                displayName = displayName,
                contentId = probe.contentId,
                updatedAtMs = System.currentTimeMillis(),
            )
            jobStore.update(job)
            updateNotification("Reading metadata…", indeterminate = true)

            if (probe.status == PkgStatus.ERROR) {
                failInstall(
                    InstallValidator.mapProbeError(probe.message),
                    probe.message ?: "Invalid package",
                )
            }
            if (probe.contentId.isBlank() && probe.titleHint.isNullOrBlank()) {
                failInstall(InstallErrorCode.NO_TITLE_ID, "Package has no title identifier")
            }

            val gamesDir = File(filesDir, "games").canonicalFile
            gamesDir.mkdirs()
            val packageBytes = probe.packageSize.coerceAtLeast(0L)
            val extractBytes = (probe.pfsImageSize.takeIf { it > 0 } ?: packageBytes).coerceAtLeast(0L)
            // Peak usage while extract holds both the local PKG cache and staging tree.
            val peak = packageBytes + extractBytes
            val margin = maxOf(STORAGE_MARGIN_BYTES, peak / 20L) // 5% or 256 MiB
            val required = peak + margin
            val free = filesDir.usableSpace
            Log.i(
                TAG,
                "pkg space package=$packageBytes extract=$extractBytes required=$required free=$free",
            )

            ImportManager.update(
                ImportProgress.CheckingStorage(probe.contentId, required, free),
            )
            job = job.copy(
                state = InstallJob.STATE_CHECKING_STORAGE,
                packageBytes = packageBytes,
                extractBytes = extractBytes,
                requiredBytes = required,
                updatedAtMs = System.currentTimeMillis(),
            )
            jobStore.update(job)

            InstallValidator.checkStorage(required, free)?.let { code ->
                failInstall(
                    code,
                    "Import needs about ${formatBytes(required)} free " +
                        "but only ${formatBytes(free)} is available",
                )
            }

            ImportManager.update(
                ImportProgress.NeedCopyConfirm(
                    contentId = probe.contentId,
                    titleHint = probe.titleHint,
                    packageBytes = packageBytes,
                    extractBytes = extractBytes,
                    requiredBytes = required,
                    freeBytes = free,
                ),
            )
            job = job.copy(
                state = InstallJob.STATE_NEED_COPY_CONFIRM,
                updatedAtMs = System.currentTimeMillis(),
            )
            jobStore.update(job)
            updateNotification("Confirm storage for PKG import", indeterminate = true)
            val confirmWaiter = CompletableDeferred<Boolean>()
            copyConfirmWaiter = confirmWaiter
            val confirmed = confirmWaiter.await()
            copyConfirmWaiter = null
            if (!confirmed) {
                throw CancellationException("cancelled")
            }
            Log.i(TAG, "pkg copy confirmed; free=$free required=$required")

            // Soft re-check after confirm (space can change while dialog is open).
            val freeNow = filesDir.usableSpace
            InstallValidator.checkStorage(required, freeNow)?.let { code ->
                failInstall(
                    code,
                    "Import needs about ${formatBytes(required)} free " +
                        "(package ${formatBytes(packageBytes)} + extract ${formatBytes(extractBytes)}) " +
                        "but only ${formatBytes(freeNow)} is available",
                )
            }

            val cacheDir = File(filesDir, "pkg-cache").canonicalFile
            cacheDir.mkdirs()
            cacheFile = File(cacheDir, "$jobId.pkg").canonicalFile
            job = job.copy(
                cachePath = "pkg-cache/$jobId.pkg",
                stagingDir = "games/.import-$jobId",
                state = InstallJob.STATE_EXTRACTING,
                updatedAtMs = System.currentTimeMillis(),
            )
            jobStore.update(job)
            Log.i(TAG, "pkg cache copy start dest=${cacheFile!!.absolutePath} size=$packageBytes")
            copyPkgToLocalCache(uri, cacheFile!!, displayName, packageBytes)
            Log.i(TAG, "pkg cache copy done size=${cacheFile!!.length()}")

            staging = File(gamesDir, ".import-$jobId").canonicalFile
            staging!!.mkdirs()
            Log.i(TAG, "pkg staging=${staging!!.absolutePath}")

            var usedPasscode: String? = null
            var extractStatus = PkgStatus.ERROR

            val candidates = buildList {
                add(null) // try embedded / zero first via native
                pkgKeyStore.getPasscode(probe.contentId)?.let { add(it) }
                add("00000000000000000000000000000000")
            }.distinct()
            Log.i(TAG, "pkg extract candidates=${candidates.size} (passcodes redacted)")

            ParcelFileDescriptor.open(cacheFile, ParcelFileDescriptor.MODE_READ_ONLY).use { localPfd ->
                val fd = localPfd.fd
                Log.i(TAG, "pkg local open ok fd=$fd size=${localPfd.statSize}")

                for ((index, candidate) in candidates.withIndex()) {
                    Log.i(
                        TAG,
                        "pkg extract attempt #$index hasPasscode=${candidate != null} " +
                            "staging=${staging!!.absolutePath}",
                    )
                    val result = withContext(Dispatchers.IO) {
                        extractWithProgress(fd, staging!!, candidate, displayName)
                    }
                    extractStatus = result.status
                    Log.i(TAG, "pkg extract attempt #$index result=${result.status} msg=${result.message}")
                    if (result.status == PkgStatus.OK) {
                        usedPasscode = candidate
                        break
                    }
                    if (result.status == PkgStatus.CANCELLED) throw CancellationException("cancelled")
                    if (result.status != PkgStatus.NEED_PASSCODE) {
                        error(result.message ?: "PKG extract failed")
                    }
                }

                if (extractStatus != PkgStatus.OK) {
                    Log.i(TAG, "pkg need user passcode contentId=${probe.contentId}")
                    ImportManager.update(
                        ImportProgress.NeedPasscode(probe.contentId, probe.titleHint),
                    )
                    updateNotification("Passcode required", indeterminate = true)
                    val waiter = CompletableDeferred<String?>()
                    passcodeWaiter = waiter
                    val userCode = waiter.await()
                    passcodeWaiter = null
                    if (userCode.isNullOrBlank()) {
                        throw CancellationException("cancelled")
                    }
                    Log.i(TAG, "pkg user passcode received len=${userCode.length}")
                    val result = withContext(Dispatchers.IO) {
                        extractWithProgress(fd, staging!!, userCode, displayName)
                    }
                    Log.i(TAG, "pkg extract with user passcode result=${result.status} msg=${result.message}")
                    if (result.status == PkgStatus.CANCELLED) throw CancellationException("cancelled")
                    if (result.status != PkgStatus.OK) {
                        error(result.message ?: "Wrong passcode or extract failed")
                    }
                    usedPasscode = userCode
                }

                ImportManager.update(ImportProgress.Verifying(displayName))
                job = job.copy(state = InstallJob.STATE_VERIFYING, updatedAtMs = System.currentTimeMillis())
                jobStore.update(job)
                updateNotification("Verifying install…", indeterminate = true)
                Log.i(TAG, "pkg finalize start")

                val sfoFile = File(staging, "sce_sys/param.sfo")
                val sfo = if (sfoFile.isFile) {
                    runCatching { ParamSfoReader.parse(sfoFile.readBytes()) }.getOrNull()
                } else {
                    null
                }
                val resolved = GameMetadataResolver.resolve(
                    folderName = displayName,
                    sfo = sfo,
                )
                Log.i(TAG, "pkg metadata id=${resolved.id} title=${resolved.title}")

                val dest = File(gamesDir, resolved.id)
                if (dest.exists()) {
                    if (GameInstallVerifier.canLaunch(filesDir, "games/${resolved.id}")) {
                        failInstall(InstallErrorCode.ALREADY_INSTALLED, "Game already installed")
                    }
                    dest.deleteRecursively()
                }

                ImportManager.update(ImportProgress.Registering(resolved.title))
                job = job.copy(
                    state = InstallJob.STATE_REGISTERING,
                    titleId = resolved.id,
                    updatedAtMs = System.currentTimeMillis(),
                )
                jobStore.update(job)
                updateNotification("Registering game…", indeterminate = true)

                val result = contentImporter.finalizeStagingTree(
                    ContentImportRequest(
                        id = resolved.id,
                        title = resolved.title,
                        sourceUri = uriString,
                        subtitle = resolved.subtitle,
                        detail = resolved.detail,
                    ),
                    staging!!,
                    mode = ImportManager.MODE_PKG,
                    contentId = probe.contentId,
                )
                // finalize moves staging; clear local ref so finally does not delete destination
                staging = null
                completed = true

                if (!usedPasscode.isNullOrBlank() &&
                    usedPasscode != "00000000000000000000000000000000" &&
                    probe.contentId.isNotBlank()
                ) {
                    pkgKeyStore.putPasscode(probe.contentId, usedPasscode)
                    Log.i(TAG, "pkg keydb saved for contentId=${probe.contentId}")
                }

                gameRepository.addImportedGame(result, uriString, System.currentTimeMillis())
                jobStore.delete(jobId)
                ImportManager.update(ImportProgress.Installed(resolved.id, resolved.title))
                Log.i(TAG, "pkg import success id=${resolved.id} bytes=${result.bytesCopied}")
                notifyDone("${resolved.title} installed")
            }
        } catch (failure: Throwable) {
            if (!completed) {
                InstallCleanup(filesDir, jobStore).cleanupJob(jobId)
            }
            handleFailure(failure)
        } finally {
            if (!completed) {
                Log.w(TAG, "pkg import cleanup staging=${staging?.absolutePath}")
                staging?.deleteRecursively()
            }
            cacheFile?.let { cached ->
                Log.i(TAG, "pkg cache delete path=${cached.absolutePath}")
                runCatching { cached.delete() }
            }
            passcodeWaiter = null
            copyConfirmWaiter = null
            if (ImportManager.isBusy()) ImportManager.reset()
            Log.i(TAG, "pkg import finished completed=$completed (stopSelf)")
            stopSelf()
        }
    }

    /**
     * Sequential stream copy from SAF/content URI into app-private storage.
     * Local random reads during extract are far faster than SAF pread.
     */
    private suspend fun copyPkgToLocalCache(
        uri: Uri,
        dest: File,
        displayName: String,
        totalHint: Long,
    ) {
        withContext(Dispatchers.IO) {
            val input = contentResolver.openInputStream(uri)
                ?: error("Cannot open PKG stream for local cache")
            input.use { stream ->
                FileOutputStream(dest).use { output ->
                    val buffer = ByteArray(COPY_BUFFER_BYTES)
                    var done = 0L
                    var lastNotifyAt = 0L
                    while (true) {
                        coroutineContext.ensureActive()
                        val n = stream.read(buffer)
                        if (n < 0) break
                        output.write(buffer, 0, n)
                        done += n
                        val now = System.currentTimeMillis()
                        if (now - lastNotifyAt >= 250L) {
                            lastNotifyAt = now
                            ImportManager.update(
                                ImportProgress.Copying(
                                    bytesCopied = done,
                                    totalBytes = totalHint,
                                    currentFile = "Local PKG cache",
                                    gameTitle = displayName,
                                ),
                            )
                            val (max, progress) = scaledProgress(done, totalHint)
                            val sizeText = if (totalHint > 0) {
                                "${formatBytes(done)} / ${formatBytes(totalHint)}"
                            } else {
                                formatBytes(done)
                            }
                            updateNotification(
                                "Copying PKG to device · $sizeText",
                                maxProgress = max,
                                progress = progress,
                                indeterminate = max == 0,
                            )
                        }
                    }
                    output.fd.sync()
                    ImportManager.update(
                        ImportProgress.Copying(
                            bytesCopied = done,
                            totalBytes = if (totalHint > 0) totalHint else done,
                            currentFile = "Local PKG cache",
                            gameTitle = displayName,
                        ),
                    )
                }
            }
        }
    }

    private fun extractWithProgress(
        fd: Int,
        staging: File,
        passcode: String?,
        displayName: String,
    ): com.bachatas4.android.runtime.pkg.PkgExtractResult {
        var lastLogAt = 0L
        Log.i(TAG, "nativeExtract enter fd=$fd out=${staging.absolutePath}")
        val result = PkgExtractor.nativeExtract(
            fd = fd,
            outPath = staging.absolutePath,
            passcode = passcode,
            listener = { bytesDone, totalHint, currentFile ->
                ImportManager.update(
                    ImportProgress.Extracting(
                        bytesCopied = bytesDone,
                        totalBytes = totalHint,
                        currentFile = currentFile,
                        gameTitle = displayName,
                    ),
                )
                val (max, progress) = scaledProgress(bytesDone, totalHint)
                val sizeText = if (totalHint > 0) {
                    "${formatBytes(bytesDone)} / ${formatBytes(totalHint)}"
                } else {
                    formatBytes(bytesDone)
                }
                updateNotification(
                    "Extracting PKG · $sizeText · $currentFile",
                    maxProgress = max,
                    progress = progress,
                    indeterminate = max == 0,
                )
                val now = System.currentTimeMillis()
                if (now - lastLogAt >= 5_000L) {
                    lastLogAt = now
                    Log.i(TAG, "nativeExtract progress $sizeText file=$currentFile")
                }
            },
        )
        Log.i(TAG, "nativeExtract exit status=${result.status} msg=${result.message}")
        return result
    }

    private fun failInstall(code: InstallErrorCode, message: String): Nothing {
        throw InstallException(code, message)
    }

    private fun handleFailure(failure: Throwable) {
        if (failure is CancellationException ||
            (failure is InstallException && failure.code == InstallErrorCode.CANCELLED)
        ) {
            Log.i(TAG, "import cancelled")
            ImportManager.update(ImportProgress.Failed(InstallErrorCode.CANCELLED, "cancelled"))
            getSystemService(NotificationManager::class.java).cancel(NOTIFICATION_ID)
            return
        }
        val (code, message) = when (failure) {
            is InstallException -> failure.code to failure.message.orEmpty()
            is ContentImportException -> {
                val mapped = when (failure.code) {
                    RuntimeErrorCode.INSUFFICIENT_STORAGE -> InstallErrorCode.INSUFFICIENT_STORAGE
                    RuntimeErrorCode.CONTENT_PERMISSION_LOST -> InstallErrorCode.PERMISSION_LOST
                    else -> InstallErrorCode.VERIFY_FAILED
                }
                mapped to (failure.message ?: failure.code.name)
            }
            else -> InstallErrorCode.UNKNOWN to (failure.message ?: failure.javaClass.simpleName)
        }
        Log.e(TAG, "install failed: $code $message", failure)
        ImportManager.update(ImportProgress.Failed(code, message))
        notifyDone("Install failed: $message", ongoing = false)
    }

    private class InstallException(
        val code: InstallErrorCode,
        message: String,
    ) : Exception(message)

    private fun notifyDone(text: String, ongoing: Boolean = false) {
        getSystemService(NotificationManager::class.java).notify(
            NOTIFICATION_ID,
            buildNotification(text, 0, 0, ongoing = ongoing),
        )
    }

    private fun createNotificationChannel() {
        getSystemService(NotificationManager::class.java).createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "Game Imports", NotificationManager.IMPORTANCE_LOW),
        )
    }

    private fun buildNotification(
        text: String,
        maxProgress: Int,
        progress: Int,
        ongoing: Boolean,
        indeterminate: Boolean = false,
    ): Notification {
        val open = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val cancel = PendingIntent.getService(
            this,
            1,
            Intent(ImportManager.ACTION_CANCEL).setClassName(packageName, ImportManager.SERVICE_CLASS),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        val builder = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_sys_download)
            .setContentTitle("Bachata S4")
            .setContentText(text)
            .setContentIntent(open)
            .setOngoing(ongoing)
            .setOnlyAlertOnce(true)
        if (ongoing) {
            builder.addAction(0, "Cancel", cancel)
            if (indeterminate || maxProgress <= 0) {
                builder.setProgress(0, 0, true)
            } else {
                builder.setProgress(maxProgress, progress.coerceIn(0, maxProgress), false)
            }
        }
        return builder.build()
    }

    private fun updateNotification(
        text: String,
        maxProgress: Int = 0,
        progress: Int = 0,
        indeterminate: Boolean = false,
    ) {
        getSystemService(NotificationManager::class.java).notify(
            NOTIFICATION_ID,
            buildNotification(
                text = text,
                maxProgress = maxProgress,
                progress = progress,
                ongoing = true,
                indeterminate = indeterminate,
            ),
        )
    }

    private companion object {
        const val TAG = "BachataImport"
        const val CHANNEL_ID = "import"
        const val NOTIFICATION_ID = 42
        const val PROGRESS_SCALE = 1000
        const val COPY_BUFFER_BYTES = 1 * 1024 * 1024
        /** Extra headroom beyond package + extract peak (256 MiB floor). */
        const val STORAGE_MARGIN_BYTES = 256L * 1024L * 1024L

        fun scaledProgress(bytesCopied: Long, totalBytes: Long): Pair<Int, Int> {
            if (totalBytes <= 0L) return 0 to 0
            val progress = ((bytesCopied.toDouble() / totalBytes.toDouble()) * PROGRESS_SCALE)
                .toInt()
                .coerceIn(0, PROGRESS_SCALE)
            return PROGRESS_SCALE to progress
        }

        fun formatBytes(bytes: Long): String {
            if (bytes < 1024) return "$bytes B"
            val kib = bytes / 1024.0
            if (kib < 1024) return "%.1f KB".format(kib)
            val mib = kib / 1024.0
            if (mib < 1024) return "%.1f MB".format(mib)
            return "%.2f GB".format(mib / 1024.0)
        }
    }
}

private fun DocumentFile.toImportEntries(): List<ContentTreeEntry> {
    val entries = mutableListOf<ContentTreeEntry>()
    val pending = ArrayDeque<Pair<DocumentFile, String>>()
    pending.add(this to "")
    while (pending.isNotEmpty()) {
        val (directory, prefix) = pending.removeLast()
        directory.listFiles().forEach { child ->
            val name = child.name ?: return@forEach
            val relativePath = if (prefix.isEmpty()) name else "$prefix/$name"
            when {
                child.isDirectory -> pending.add(child to relativePath)
                child.isFile -> entries.add(
                    ContentTreeEntry(relativePath, child.uri.toString(), child.length().coerceAtLeast(0L)),
                )
            }
        }
    }
    return entries
}
