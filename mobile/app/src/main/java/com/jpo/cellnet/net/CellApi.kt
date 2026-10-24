package com.jpo.cellnet.net

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException
import java.util.concurrent.TimeUnit

/** A non 2xx answer from the local station, with its machine readable status. */
class ApiException(
    val code: Int,
    val status: String,
    val detail: String?
) : IOException("HTTP $code $status" + (detail?.let { ": $it" } ?: ""))

data class CellInfo(
    val site: String,
    val ssid: String,
    val node: Int,
    val ip: String,
    val httpPort: Int,
    val wsPort: Int
)

data class DirectoryUser(
    val id: String,
    val name: String,
    val station: String,
    val online: Boolean
)

/**
 * Known cells. The IP addresses match the static access point addresses
 * configured in platformio.ini (SITE_A 10.0.0.1, SITE_B 10.1.0.1,
 * SITE_C 10.2.0.1). Change both sides together.
 */
object Cells {
    data class Cell(
        val ssid: String,
        val site: String,
        val ip: String,
        val password: String,
        val httpPort: Int = 8080,
        val wsPort: Int = 8081
    )

    val all: List<Cell> = listOf(
        Cell("CELL_SITE_A", "SITE_A", "10.0.0.1", "cellsitea"),
        Cell("CELL_SITE_B", "SITE_B", "10.1.0.1", "cellsiteb"),
        Cell("CELL_SITE_C", "SITE_C", "10.2.0.1", "cellsitec")
    )

    fun bySsid(ssid: String?): Cell? =
        all.firstOrNull { it.ssid.equals(ssid?.trim(), ignoreCase = true) }

    fun bySite(site: String?): Cell? = all.firstOrNull { it.site == site }

    /** Cell whose access point uses this IPv4 address (DHCP gateway of the AP). */
    fun byIp(ip: String?): Cell? {
        val value = ip?.trim() ?: return null
        if (value.isEmpty()) return null
        return all.firstOrNull { it.ip == value }
    }
}

/** Thin blocking HTTP/JSON client for the local REST API of a station. */
class CellApi {

    private val client = OkHttpClient.Builder()
        .connectTimeout(4, TimeUnit.SECONDS)
        .readTimeout(12, TimeUnit.SECONDS)
        .writeTimeout(12, TimeUnit.SECONDS)
        .build()

    @Volatile
    var baseUrl: String = ""

    suspend fun info(): CellInfo = withContext(Dispatchers.IO) {
        val json = JSONObject(get("/api/info"))
        CellInfo(
            site = json.optString("site"),
            ssid = json.optString("ssid"),
            node = json.optInt("node"),
            ip = json.optString("ip"),
            httpPort = json.optInt("httpPort", 8080),
            wsPort = json.optInt("wsPort", 8081)
        )
    }

    suspend fun users(): List<DirectoryUser> = withContext(Dispatchers.IO) {
        val array = JSONObject(get("/api/users")).optJSONArray("users") ?: JSONArray()
        (0 until array.length()).mapNotNull { index ->
            val item = array.optJSONObject(index) ?: return@mapNotNull null
            val id = item.optString("id")
            if (id.isBlank()) return@mapNotNull null
            DirectoryUser(
                id = id,
                name = item.optString("name", id),
                station = item.optString("station", ""),
                online = item.optBoolean("online", false)
            )
        }
    }

    suspend fun registerPresence(userId: String, displayName: String): JSONObject =
        withContext(Dispatchers.IO) {
            post(
                "/api/presence",
                JSONObject()
                    .put("userId", userId)
                    .put("displayName", displayName)
                    .toString()
            )
        }

    suspend fun leave(userId: String): JSONObject = withContext(Dispatchers.IO) {
        post("/api/presence/leave", JSONObject().put("userId", userId).toString())
    }

    suspend fun sendText(json: String): JSONObject = withContext(Dispatchers.IO) {
        post("/api/messages/text", json)
    }

    suspend fun sendReceipt(json: String): JSONObject = withContext(Dispatchers.IO) {
        post("/api/messages/receipt", json)
    }

    suspend fun sendVoiceBlock(json: String): JSONObject = withContext(Dispatchers.IO) {
        post("/api/voice/block", json)
    }

    suspend fun sendVoiceFeedback(json: String): JSONObject = withContext(Dispatchers.IO) {
        post("/api/voice/feedback", json)
    }

    // -----------------------------------------------------------------------
    // Live calls. Signalling only: the audio itself travels as binary WebSocket
    // frames, never through these endpoints and never through the RS485 bus.
    // -----------------------------------------------------------------------

    suspend fun callInvite(json: String): JSONObject = withContext(Dispatchers.IO) {
        post("/api/call/invite", json)
    }

    suspend fun callAccept(json: String): JSONObject = withContext(Dispatchers.IO) {
        post("/api/call/accept", json)
    }

    suspend fun callReject(json: String): JSONObject = withContext(Dispatchers.IO) {
        post("/api/call/reject", json)
    }

    suspend fun callCancel(json: String): JSONObject = withContext(Dispatchers.IO) {
        post("/api/call/cancel", json)
    }

    suspend fun callHangup(json: String): JSONObject = withContext(Dispatchers.IO) {
        post("/api/call/hangup", json)
    }

    /** Diagnostic state of the calls of this cell. */
    suspend fun callState(userId: String): JSONObject = withContext(Dispatchers.IO) {
        JSONObject(get("/api/call/state?userId=$userId"))
    }

    private fun get(path: String): String {
        if (baseUrl.isBlank()) throw IOException("station URL not configured")
        val request = Request.Builder().url(baseUrl + path).get().build()
        return client.newCall(request).execute().use { response ->
            val text = response.body?.string().orEmpty()
            if (!response.isSuccessful) throw toException(response.code, text)
            text
        }
    }

    private fun post(path: String, body: String): JSONObject {
        if (baseUrl.isBlank()) throw IOException("station URL not configured")
        val request = Request.Builder()
            .url(baseUrl + path)
            .post(body.toRequestBody(JSON))
            .build()
        return client.newCall(request).execute().use { response ->
            val text = response.body?.string().orEmpty()
            if (!response.isSuccessful) throw toException(response.code, text)
            runCatching { JSONObject(text) }.getOrNull()
                ?: throw IOException("invalid JSON answer from station")
        }
    }

    private fun toException(code: Int, text: String): ApiException {
        val json = runCatching { JSONObject(text) }.getOrNull()
        return ApiException(
            code = code,
            status = json?.optString("status")?.ifBlank { null } ?: "http-$code",
            detail = json?.optString("detail")?.ifBlank { null }
        )
    }

    companion object {
        private val JSON = "application/json; charset=utf-8".toMediaType()
    }
}
