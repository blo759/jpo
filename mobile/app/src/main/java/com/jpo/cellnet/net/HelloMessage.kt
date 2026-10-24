package com.jpo.cellnet.net

import java.util.Locale

/**
 * The hello message binds a socket to a user. Two things matter:
 *
 *  * the `userId` is the stable identity, never the name, the IP address or the
 *    cell the phone is attached to, so a user that moves to another cell is
 *    recognised as the same person instead of being created again;
 *  * the `displayName` travels with it, because the station that receives the
 *    user for the first time has no other way to learn its name. Without it the
 *    user would be registered under its raw identifier on the new cell and every
 *    other cell would then show "u-3f2a1b9c0d" instead of the announced name.
 */
object HelloMessage {

    fun build(userId: String, displayName: String): String {
        val name = displayName.trim().ifBlank { userId }
        return "{\"type\":\"hello\",\"userId\":${quote(userId)},\"displayName\":${quote(name)}}"
    }

    private fun quote(value: String): String {
        val out = StringBuilder(value.length + 2)
        out.append('"')
        for (ch in value) {
            when {
                ch == '"' -> out.append("\\\"")
                ch == '\\' -> out.append("\\\\")
                ch == '\n' -> out.append("\\n")
                ch == '\r' -> out.append("\\r")
                ch == '\t' -> out.append("\\t")
                ch.code < 0x20 -> out.append(String.format(Locale.ROOT, "\\u%04x", ch.code))
                else -> out.append(ch)
            }
        }
        out.append('"')
        return out.toString()
    }
}
