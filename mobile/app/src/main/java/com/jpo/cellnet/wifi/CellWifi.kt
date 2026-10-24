package com.jpo.cellnet.wifi

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.location.LocationManager
import android.net.ConnectivityManager
import android.net.Network
import android.net.NetworkCapabilities
import android.net.NetworkRequest
import android.net.wifi.WifiConfiguration
import android.net.wifi.WifiManager
import android.net.wifi.WifiNetworkSpecifier
import android.os.Build
import android.util.Log
import androidx.core.content.ContextCompat
import com.jpo.cellnet.net.Cells
import java.net.Inet4Address

/** Why the SSID could not be read (a null SSID is never turned into a cell name). */
enum class SsidStatus { AVAILABLE, NO_PERMISSION, REDACTED, EMPTY, ERROR }

/**
 * Immutable view of the Wi-Fi state at one point in time.
 *
 * `transportIsWifi` answers "is the active connection a Wi-Fi network" and is
 * independent from `ssid` (which Android may redact). `gatewayIp` is the IPv4
 * gateway of the Wi-Fi link, i.e. the ESP32 access point address announced by
 * its DHCP server; it is used to identify the cell when the SSID is unreadable.
 */
data class WifiSnapshot(
    val sdkInt: Int,
    val targetSdk: Int,
    val wifiEnabled: Boolean,
    val transportIsWifi: Boolean,
    val ssid: String?,
    val ssidStatus: SsidStatus,
    val gatewayIp: String?,
    val network: Network?,
    val hasFineLocation: Boolean,
    val hasNearbyWifi: Boolean,
    val locationServicesEnabled: Boolean,
    val error: String? = null
) {
    val canAttemptSsid: Boolean get() = hasFineLocation || hasNearbyWifi

    /**
     * True when the phone is associated to a Wi-Fi network. `transportIsWifi`
     * alone is not enough: when the access point has no Internet (the ESP32
     * case) Android may keep mobile data as the validated default network, so
     * the active network is not Wi-Fi even though the phone is on the AP.
     */
    val connectedToWifi: Boolean get() = transportIsWifi || ssid != null


    fun describe(): String = buildString {
        append("sdk=").append(sdkInt)
        append(" targetSdk=").append(targetSdk)
        append(" fineLocation=").append(hasFineLocation)
        append(" nearbyWifi=").append(hasNearbyWifi)
        append(" locationServices=").append(locationServicesEnabled)
        append(" wifiEnabled=").append(wifiEnabled)
        append(" transportWifi=").append(transportIsWifi)
        append(" ssidStatus=").append(ssidStatus)
        append(" ssid=").append(ssid ?: "<null>")
        append(" gateway=").append(gatewayIp ?: "<null>")
        if (error != null) append(" error=").append(error)
    }
}

/**
 * Wi-Fi detection and cell connection helper.
 *
 * Detection is split in three independent steps:
 *  1. is the active connection Wi-Fi? ([WifiSnapshot.transportIsWifi])
 *  2. what is the SSID, or why is it unavailable? ([WifiSnapshot.ssidStatus])
 *  3. which cell does it correspond to? Done by the caller from the SSID and the
 *     gateway, then confirmed by the station REST API.
 */
class CellWifi(private val context: Context) {

    private val appContext = context.applicationContext
    private val wifiManager =
        appContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
    private val connectivity =
        appContext.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
    private val locationManager =
        appContext.getSystemService(Context.LOCATION_SERVICE) as? LocationManager

    /** Callback of an explicit WifiNetworkSpecifier request. */
    private var requestCallback: ConnectivityManager.NetworkCallback? = null

    /** Callback observing Wi-Fi connect/disconnect events. */
    private var monitorCallback: ConnectivityManager.NetworkCallback? = null

    @Volatile
    private var boundNetwork: Network? = null

    @Volatile
    private var monitoredWifiNetwork: Network? = null

    // -----------------------------------------------------------------------
    // Permissions
    // -----------------------------------------------------------------------
    fun requiredPermissions(): Array<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            arrayOf(
                Manifest.permission.NEARBY_WIFI_DEVICES,
                Manifest.permission.ACCESS_FINE_LOCATION
            )
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

    fun missingPermissions(): List<String> =
        requiredPermissions().filterNot { granted(it) }

    fun hasFineLocation(): Boolean = granted(Manifest.permission.ACCESS_FINE_LOCATION)

    fun hasNearbyWifi(): Boolean =
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            granted(Manifest.permission.NEARBY_WIFI_DEVICES)

    private fun granted(permission: String): Boolean =
        ContextCompat.checkSelfPermission(appContext, permission) == PackageManager.PERMISSION_GRANTED

    fun canAttemptSsidRead(): Boolean = hasFineLocation() || hasNearbyWifi()

    // -----------------------------------------------------------------------
    // Snapshot
    // -----------------------------------------------------------------------
    fun snapshot(): WifiSnapshot {
        val targetSdk = appContext.applicationInfo.targetSdkVersion
        val wifiEnabled = runCatching { wifiManager.isWifiEnabled }.getOrDefault(false)
        val wifiNetwork = activeWifiNetwork()
        val ssidResult = readSsid()
        return WifiSnapshot(
            sdkInt = Build.VERSION.SDK_INT,
            targetSdk = targetSdk,
            wifiEnabled = wifiEnabled,
            transportIsWifi = wifiNetwork != null,
            ssid = ssidResult.first,
            ssidStatus = ssidResult.second,
            gatewayIp = wifiNetwork?.let { gatewayOf(it) },
            network = wifiNetwork,
            hasFineLocation = hasFineLocation(),
            hasNearbyWifi = hasNearbyWifi(),
            locationServicesEnabled = locationServicesEnabled(),
            error = ssidResult.third
        )
    }

    /**
     * Active Wi-Fi network. A network bound by [connectToCell] or [bindToActiveWifi]
     * is checked first: with WifiNetworkSpecifier the system keeps `activeNetwork`
     * on mobile data while our process is bound to the access point.
     */
    @Suppress("DEPRECATION")
    private fun activeWifiNetwork(): Network? {
        val bound = boundNetwork
        if (bound != null && isWifi(bound)) {
            return bound
        }
        val active = connectivity.activeNetwork
        if (active != null && isWifi(active)) {
            return active
        }
        // The ESP32 access point has no Internet: Android keeps mobile data as the
        // validated default network while the phone is still associated to the AP.
        // Look at every network known to the system, then at the one reported by
        // our own Wi-Fi callback.
        val known = runCatching { connectivity.allNetworks.firstOrNull { isWifi(it) } }
            .getOrNull()
        return known ?: monitoredWifiNetwork
    }

    private fun isWifi(network: Network): Boolean = runCatching {
        connectivity.getNetworkCapabilities(network)
            ?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI) == true
    }.getOrDefault(false)

    /** IPv4 gateway announced by the access point (the ESP32 address). */
    private fun gatewayOf(network: Network): String? {
        val properties = runCatching { connectivity.getLinkProperties(network) }.getOrNull()
            ?: return null
        val route = properties.routes.firstOrNull { it.isDefaultRoute && it.gateway is Inet4Address }
            ?: properties.routes.firstOrNull { it.gateway is Inet4Address }
        return route?.gateway?.hostAddress
    }

    @Suppress("DEPRECATION")
    private fun readSsid(): Triple<String?, SsidStatus, String?> {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O_MR1 && !canAttemptSsidRead()) {
            return Triple(null, SsidStatus.NO_PERMISSION, "permission Wi-Fi / localisation non accordée")
        }
        val raw = try {
            wifiManager.connectionInfo?.ssid
        } catch (security: SecurityException) {
            Log.w(TAG, "SecurityException while reading the SSID", security)
            return Triple(null, SsidStatus.NO_PERMISSION, "SecurityException: ${security.message}")
        } catch (error: Exception) {
            Log.w(TAG, "cannot read WifiInfo", error)
            return Triple(null, SsidStatus.ERROR, "${error.javaClass.simpleName}: ${error.message}")
        }

        val clean = cleanSsid(raw)
        return when {
            clean != null -> Triple(clean, SsidStatus.AVAILABLE, null)
            raw.isNullOrBlank() -> Triple(null, SsidStatus.EMPTY, "WifiInfo.ssid vide")
            else -> Triple(null, SsidStatus.REDACTED, "SSID masqué par Android (${raw.trim()})")
        }
    }

    private fun cleanSsid(raw: String?): String? {
        if (raw == null) return null
        var value = raw.trim()
        if (value.length >= 2 && value.startsWith("\"") && value.endsWith("\"")) {
            value = value.substring(1, value.length - 1)
        }
        if (value.isBlank()) return null
        if (value.contains("unknown", ignoreCase = true)) return null
        if (value == "0x") return null
        return value
    }

    @Suppress("DEPRECATION")
    fun locationServicesEnabled(): Boolean = runCatching {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            locationManager?.isLocationEnabled == true
        } else {
            locationManager?.isProviderEnabled(LocationManager.GPS_PROVIDER) == true ||
                locationManager?.isProviderEnabled(LocationManager.NETWORK_PROVIDER) == true
        }
    }.getOrDefault(false)

    fun isWifiEnabled(): Boolean = runCatching { wifiManager.isWifiEnabled }.getOrDefault(false)

    // -----------------------------------------------------------------------
    // Routing: keep the local station reachable even without Internet
    // -----------------------------------------------------------------------
    /**
     * Binds the process to the current Wi-Fi network. Without this, Android keeps
     * sending our sockets through the validated default network (mobile data) and
     * the ESP32 at 10.x.x.x is unreachable, even though the phone is connected to
     * its access point. This does not disable any security setting.
     */
    fun bindToActiveWifi(): Network? {
        val network = activeWifiNetwork() ?: return null
        return runCatching {
            if (connectivity.bindProcessToNetwork(network)) {
                boundNetwork = network
                Log.i(TAG, "process bound to the Wi-Fi network")
                network
            } else {
                Log.w(TAG, "bindProcessToNetwork returned false")
                null
            }
        }.getOrElse { error ->
            Log.w(TAG, "bindProcessToNetwork failed", error)
            null
        }
    }

    fun unbindProcess() {
        if (boundNetwork == null) return
        runCatching { connectivity.bindProcessToNetwork(null) }
        boundNetwork = null
        Log.i(TAG, "process unbound from the Wi-Fi network")
    }

    // -----------------------------------------------------------------------
    // Network change monitoring (no polling scan)
    // -----------------------------------------------------------------------
    fun startMonitoring(onChange: () -> Unit) {
        if (monitorCallback != null) return
        val callback = object : ConnectivityManager.NetworkCallback() {
            override fun onAvailable(network: Network) {
                monitoredWifiNetwork = network
                Log.i(TAG, "Wi-Fi network available")
                onChange()
            }

            override fun onLost(network: Network) {
                if (monitoredWifiNetwork == network) {
                    monitoredWifiNetwork = null
                }
                Log.i(TAG, "Wi-Fi network lost")
                onChange()
            }

            override fun onCapabilitiesChanged(
                network: Network,
                networkCapabilities: NetworkCapabilities
            ) {
                onChange()
            }
        }
        val request = NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .build()
        runCatching { connectivity.registerNetworkCallback(request, callback) }
            .onSuccess {
                monitorCallback = callback
                Log.i(TAG, "Wi-Fi monitoring started")
            }
            .onFailure { Log.w(TAG, "cannot register the Wi-Fi callback", it) }
    }

    fun stopMonitoring() {
        monitorCallback?.let { callback ->
            runCatching { connectivity.unregisterNetworkCallback(callback) }
        }
        monitorCallback = null
        monitoredWifiNetwork = null
    }

    // -----------------------------------------------------------------------
    // Nearby cells (explicit user action only)
    // -----------------------------------------------------------------------
    fun scan(): Boolean = runCatching { wifiManager.startScan() }.getOrDefault(false)

    @Suppress("DEPRECATION")
    fun nearbyCells(): List<Pair<String, Int>> {
        if (!canAttemptSsidRead()) return emptyList()
        val known = Cells.all.map { it.ssid }.toSet()
        val results = runCatching { wifiManager.scanResults }.getOrNull().orEmpty()
        return results.mapNotNull { result ->
            val ssid = cleanSsid(result.SSID) ?: return@mapNotNull null
            if (ssid !in known) return@mapNotNull null
            ssid to result.level
        }.distinctBy { it.first }.sortedByDescending { it.second }
    }

    /**
     * Asks Android to connect to the given cell access point. [onState] receives
     * "requesting", "connected", "unavailable", "lost" or "error:<message>".
     * On API 29+ the system shows its own confirmation dialog.
     */
    fun connectToCell(cell: Cells.Cell, onState: (String) -> Unit) {
        release()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val specifier = WifiNetworkSpecifier.Builder()
                .setSsid(cell.ssid)
                .setWpa2Passphrase(cell.password)
                .build()
            val request = NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .setNetworkSpecifier(specifier)
                .build()

            val callback = object : ConnectivityManager.NetworkCallback() {
                override fun onAvailable(network: Network) {
                    boundNetwork = network
                    connectivity.bindProcessToNetwork(network)
                    onState("connected")
                }

                override fun onUnavailable() {
                    onState("unavailable")
                }

                override fun onLost(network: Network) {
                    if (boundNetwork == network) {
                        connectivity.bindProcessToNetwork(null)
                        boundNetwork = null
                    }
                    onState("lost")
                }
            }
            requestCallback = callback
            onState("requesting")
            runCatching { connectivity.requestNetwork(request, callback, 60_000) }
                .onFailure { onState("error:${it.message ?: "requestNetwork"}") }
            return
        }

        @Suppress("DEPRECATION")
        val configuration = WifiConfiguration().apply {
            SSID = "\"${cell.ssid}\""
            preSharedKey = "\"${cell.password}\""
        }
        @Suppress("DEPRECATION")
        val networkId = runCatching { wifiManager.addNetwork(configuration) }.getOrDefault(-1)
        if (networkId < 0) {
            onState("unavailable")
            return
        }
        onState("requesting")
        runCatching {
            wifiManager.disconnect()
            wifiManager.enableNetwork(networkId, true)
            wifiManager.reconnect()
            onState("connected")
        }.onFailure { onState("error:${it.message ?: "legacy connect"}") }
    }

    /** Releases the explicit request and restores the default routing. */
    fun release() {
        requestCallback?.let { callback ->
            runCatching { connectivity.unregisterNetworkCallback(callback) }
        }
        requestCallback = null
        unbindProcess()
    }

    companion object {
        private const val TAG = "CellNetWifi"
    }
}
