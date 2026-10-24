package com.jpo.cellnet.sync

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Durable outbox policy: a temporary refusal must be retried with a growing
 * delay, a permanent refusal must never be retried silently.
 */
class OutboxPolicyTest {

    @Test
    fun delayGrowsThenStaysCapped() {
        assertEquals(0L, OutboxPolicy.delayFor(0))
        assertEquals(OutboxPolicy.BASE_DELAY_MS, OutboxPolicy.delayFor(1))
        assertTrue(OutboxPolicy.delayFor(2) > OutboxPolicy.delayFor(1))
        assertTrue(OutboxPolicy.delayFor(3) > OutboxPolicy.delayFor(2))
        assertEquals(OutboxPolicy.MAX_DELAY_MS, OutboxPolicy.delayFor(10))
        assertEquals(OutboxPolicy.MAX_DELAY_MS, OutboxPolicy.delayFor(99))
    }

    @Test
    fun attemptsAreBounded() {
        assertTrue(OutboxPolicy.shouldRetry(0))
        assertTrue(OutboxPolicy.shouldRetry(OutboxPolicy.MAX_ATTEMPTS - 1))
        assertFalse(OutboxPolicy.shouldRetry(OutboxPolicy.MAX_ATTEMPTS))
        assertFalse(OutboxPolicy.shouldRetry(OutboxPolicy.MAX_ATTEMPTS + 5))
    }

    @Test
    fun temporaryRefusalsAreRetried() {
        assertTrue(OutboxPolicy.retryable("busy"))
        assertTrue(OutboxPolicy.retryable("recipient-offline"))
        assertTrue(OutboxPolicy.retryable("unknown-recipient"))
        assertTrue(OutboxPolicy.retryable("http-503"))
        assertTrue(OutboxPolicy.retryable("http-500"))
        assertTrue(OutboxPolicy.retryable("timeout"))
    }

    @Test
    fun permanentRefusalsAreNotRetried() {
        assertFalse(OutboxPolicy.retryable("too-large"))
        assertFalse(OutboxPolicy.retryable("missing-fields"))
        assertFalse(OutboxPolicy.retryable("invalid-json"))
        assertFalse(OutboxPolicy.retryable("empty-body"))
        assertFalse(OutboxPolicy.retryable("http-400"))
        assertFalse(OutboxPolicy.retryable("http-413"))
    }

    @Test
    fun permanentReasonIsHumanReadable() {
        assertTrue(OutboxPolicy.permanentReason("too-large").contains("trop long"))
        assertTrue(OutboxPolicy.permanentReason("presence-table-full").contains("annuaire"))
        assertTrue(OutboxPolicy.permanentReason("something-else").isNotBlank())
    }
}
