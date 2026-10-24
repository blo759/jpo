package com.jpo.cellnet.data

import android.content.Context
import java.util.UUID

data class UserProfile(val userId: String, val displayName: String)

/**
 * Local identity storage. The display name is only a label: the stable
 * [UserProfile.userId] is what the routing table uses. There is no account
 * server in this prototype, therefore the id is not an authentication proof.
 */
class UserProfileStore(context: Context) {

    private val prefs =
        context.applicationContext.getSharedPreferences("cellnet_profile", Context.MODE_PRIVATE)

    fun load(): UserProfile? {
        val id = prefs.getString(KEY_USER_ID, null) ?: return null
        val name = prefs.getString(KEY_DISPLAY_NAME, null) ?: return null
        if (id.isBlank() || name.isBlank()) return null
        return UserProfile(id, name)
    }

    fun save(displayName: String): UserProfile {
        val existingId = prefs.getString(KEY_USER_ID, null)
        val id = existingId ?: generateUserId()
        val cleanName = displayName.trim().ifBlank { "Utilisateur" }
        prefs.edit()
            .putString(KEY_USER_ID, id)
            .putString(KEY_DISPLAY_NAME, cleanName)
            .apply()
        return UserProfile(id, cleanName)
    }

    fun rename(displayName: String): UserProfile? {
        val current = load() ?: return null
        val cleanName = displayName.trim()
        if (cleanName.isBlank()) return null
        prefs.edit().putString(KEY_DISPLAY_NAME, cleanName).apply()
        return UserProfile(current.userId, cleanName)
    }

    fun clear() {
        prefs.edit().clear().apply()
    }

    private fun generateUserId(): String =
        "u-" + UUID.randomUUID().toString().replace("-", "").take(10)

    companion object {
        private const val KEY_USER_ID = "userId"
        private const val KEY_DISPLAY_NAME = "displayName"
    }
}
