package com.jpo.cellnet.net

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Client side guard of the user identity.
 *
 * Regression: the WebSocket hello used to carry only the identifier, so the
 * station that received a user for the first time had no display name to register
 * and fell back to the raw identifier. A user moving from cell A to cell B was
 * then displayed as "u-3f2a1b9c0d" on every other cell, and looked like a
 * different person.
 *
 * The identity itself must stay the stable identifier: never the name, never the
 * cell, never the address.
 */
class HelloMessageTest {

    @Test
    fun `the hello carries the stable identifier and the display name`() {
        assertEquals(
            "{\"type\":\"hello\",\"userId\":\"u-3f2a1b9c0d\",\"displayName\":\"Abdou\"}",
            HelloMessage.build("u-3f2a1b9c0d", "Abdou")
        )
    }

    @Test
    fun `the identifier is never replaced by the display name`() {
        val message = HelloMessage.build("u-1", "Abdou")
        assertTrue(message.contains("\"userId\":\"u-1\""))
        assertTrue(message.contains("\"displayName\":\"Abdou\""))
    }

    @Test
    fun `a blank display name falls back to the identifier instead of being empty`() {
        for (blank in listOf("", "   ", "\t")) {
            assertEquals(
                "{\"type\":\"hello\",\"userId\":\"u-42\",\"displayName\":\"u-42\"}",
                HelloMessage.build("u-42", blank)
            )
        }
    }

    @Test
    fun `a name with quotes or backslashes is escaped so the json stays valid`() {
        assertEquals(
            "{\"type\":\"hello\",\"userId\":\"u-7\"," +
                "\"displayName\":\"Abdou \\\"Le Grand\\\" \\\\ Diallo\\nCellule B\"}",
            HelloMessage.build("u-7", "Abdou \"Le Grand\" \\ Diallo\nCellule B")
        )
    }

    @Test
    fun `a control character is escaped`() {
        assertEquals(
            "{\"type\":\"hello\",\"userId\":\"u-1\",\"displayName\":\"a\\u0001b\"}",
            HelloMessage.build("u-1", "a\u0001b")
        )
    }

    @Test
    fun `the payload stays a single line and bounded`() {
        val message = HelloMessage.build("u-1", "Abdou")
        assertFalse(message.contains('\n'))
        assertFalse(message.contains('\r'))
        // A hello frame must never approach the link payload limit.
        assertTrue(message.length < 120)
    }
}
