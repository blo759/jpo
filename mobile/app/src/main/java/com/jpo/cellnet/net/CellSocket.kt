package com.jpo.cellnet.net

import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import java.util.concurrent.TimeUnit

/**
 * Persistent WebSocket channel used for real time events. The first frame sent
 * after the connection is the hello message binding the socket to a user id.
 */
class CellSocket(private val listener: Listener) {

    interface Listener {
        fun onSocketOpen()
        fun onSocketText(text: String)
        fun onSocketBinary(bytes: ByteArray)
        fun onSocketClosed(reason: String)
    }

    private val client = OkHttpClient.Builder()
        .connectTimeout(5, TimeUnit.SECONDS)
        .readTimeout(0, TimeUnit.MILLISECONDS)
        .pingInterval(20, TimeUnit.SECONDS)
        .build()

    private var socket: WebSocket? = null

    @Volatile
    private var connected = false

    fun isConnected(): Boolean = connected

    fun connect(url: String, userId: String) {
        disconnect()
        val request = Request.Builder().url(url).build()
        socket = client.newWebSocket(request, object : WebSocketListener() {
            override fun onOpen(webSocket: WebSocket, response: Response) {
                connected = true
                webSocket.send("{\"type\":\"hello\",\"userId\":\"$userId\"}")
                listener.onSocketOpen()
            }

            override fun onMessage(webSocket: WebSocket, text: String) {
                listener.onSocketText(text)
            }

            override fun onMessage(webSocket: WebSocket, bytes: ByteString) {
                listener.onSocketBinary(bytes.toByteArray())
            }

            override fun onClosing(webSocket: WebSocket, code: Int, reason: String) {
                webSocket.close(1000, null)
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                connected = false
                listener.onSocketClosed(reason.ifBlank { "closed" })
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                connected = false
                listener.onSocketClosed(t.message ?: "connection failure")
            }
        })
    }

    fun disconnect() {
        connected = false
        socket?.close(1000, "bye")
        socket = null
    }
}
