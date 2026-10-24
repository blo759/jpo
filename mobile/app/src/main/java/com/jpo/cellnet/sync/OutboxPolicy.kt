package com.jpo.cellnet.sync

/**
 * Retry policy of the durable outbox.
 *
 * The message states live in Room, so an application restart never loses a
 * pending message. This policy decides *when* a message is tried again and
 * whether a station answer is worth retrying at all:
 *   * a temporary refusal (`busy`, `recipient-offline`, transport error) is
 *     retried with an exponential backoff,
 *   * a permanent refusal (`too-large`, malformed body) is never retried: the
 *     user gets an explicit error instead of an endless silent loop.
 *
 * Pure Kotlin: unit tested on the JVM without Android.
 */
object OutboxPolicy {

    const val BASE_DELAY_MS = 2_000L
    const val MAX_DELAY_MS = 60_000L
    const val MAX_ATTEMPTS = 8

    /** Exponential backoff, capped, so a saturated station is not hammered. */
    fun delayFor(attempt: Int): Long {
        if (attempt <= 0) return 0L
        var delay = BASE_DELAY_MS
        var remaining = attempt - 1
        while (remaining > 0 && delay < MAX_DELAY_MS) {
            delay *= 2
            remaining--
        }
        return minOf(delay, MAX_DELAY_MS)
    }

    fun shouldRetry(attempt: Int): Boolean = attempt < MAX_ATTEMPTS

    /**
     * Statuses returned by the station that are worth another attempt. Anything
     * else (a malformed payload, a message above the supported size) is a
     * permanent failure and must be shown to the user.
     */
    fun retryable(status: String): Boolean = when (status) {
        "busy",
        "recipient-offline",
        "unknown-recipient",
        "station-unreachable",
        "not-configured",
        "timeout",
        "invalid-json-response",
        "http-500",
        "http-502",
        "http-503",
        "http-504" -> true
        else -> status.startsWith("http-5")
    }

    /** Human readable reason shown when a message is finally abandoned. */
    fun permanentReason(status: String): String = when (status) {
        "too-large" -> "message trop long pour le réseau"
        "missing-fields", "missing-user-id" -> "message incomplet"
        "invalid-json", "empty-body", "empty-block" -> "message invalide"
        "invalid-sequence" -> "séquence de bloc invalide"
        "presence-table-full" -> "annuaire de la cellule saturé"
        else -> "refusé par la station ($status)"
    }
}
