#include "configuration.h"
#if defined(ARCH_ESP32) && !MESHTASTIC_EXCLUDE_PAXCOUNTER
#include "Default.h"
#include "MeshService.h"
#include "PaxcounterModule.h"
#include <assert.h>

PaxcounterModule* paxcounterModule;

/**
 * Callback function for libpax.
 * We only clear our sent flag here, since this function is called from another thread, so we
 * cannot send to the mesh directly.
 */
void PaxcounterModule::handlePaxCounterReportRequest()
{
    // Safety check - if module pointer is null, don't proceed
    if (!paxcounterModule)
    {
        LOG_ERROR("PaxcounterModule: Callback called but module is null!");
        return;
    }

    LOG_DEBUG("PaxcounterModule: Starting handlePaxCounterReportRequest");

    // The libpax library already updated our data structure, just before invoking this callback.
    LOG_INFO("PaxcounterModule: libpax reported new data: wifi=%d; ble=%d; uptime=%lu",
             paxcounterModule->count_from_libpax.wifi_count,
             paxcounterModule->count_from_libpax.ble_count,
             millis() / 1000);

    // Make sure we don't access devices_from_libpax.devices if it's null
    LOG_DEBUG("PaxcounterModule: Current device list - count: %d, capacity: %d, devices ptr: %p",
              paxcounterModule->devices_from_libpax.count,
              paxcounterModule->devices_from_libpax.capacity,
              paxcounterModule->devices_from_libpax.devices);

    // Reset flags to trigger sending
    paxcounterModule->reportedDataSent = false;

    // Only set deviceListSent to false if we actually have devices
    if (paxcounterModule->devices_from_libpax.devices != nullptr &&
        paxcounterModule->devices_from_libpax.count > 0)
    {
        paxcounterModule->deviceListSent = false;
        LOG_DEBUG("PaxcounterModule: Reset deviceListSent flag");
    }
    else
    {
        LOG_DEBUG("PaxcounterModule: No valid devices, keeping deviceListSent=true");
    }

    LOG_DEBUG("PaxcounterModule: About to call setIntervalFromNow(0)");

    // Schedule the module to run soon
    paxcounterModule->setIntervalFromNow(0);

    LOG_DEBUG("PaxcounterModule: Completed handlePaxCounterReportRequest");
}

PaxcounterModule::PaxcounterModule()
    : concurrency::OSThread("Paxcounter"),
      ProtobufModule("paxcounter", meshtastic_PortNum_PAXCOUNTER_APP, &meshtastic_Paxcount_msg)
{
}

/**
 * Send the Pax information to the mesh if we got new data from libpax.
 * This is called periodically from our runOnce() method and will actually send the data to the mesh
 * if libpax updated it since the last transmission through the callback.
 * @param dest - destination node (usually NODENUM_BROADCAST)
 * @return false if sending is unnecessary, true if information was sent
 */
bool PaxcounterModule::sendInfo(NodeNum dest)
{
    if (paxcounterModule->reportedDataSent)
        return false;

    LOG_INFO("PaxcounterModule: send pax info wifi=%d; ble=%d; uptime=%lu", count_from_libpax.wifi_count,
             count_from_libpax.ble_count, millis() / 1000);

    meshtastic_Paxcount pl = meshtastic_Paxcount_init_default;
    pl.wifi = count_from_libpax.wifi_count;
    pl.ble = count_from_libpax.ble_count;
    pl.uptime = millis() / 1000;

    meshtastic_MeshPacket* p = allocDataProtobuf(pl);
    p->to = dest;
    p->decoded.want_response = false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;

    service->sendToMesh(p, RX_SRC_LOCAL, true);

    paxcounterModule->reportedDataSent = true;

    return true;
}

/**
 * Send the detailed device list to the mesh if we got new data from libpax.
 * @param dest - destination node (usually NODENUM_BROADCAST)
 * @return false if sending is unnecessary, true if information was sent
 */
bool PaxcounterModule::sendDeviceList(NodeNum dest)
{
    LOG_DEBUG("PaxcounterModule: Starting sendDeviceList function");

    if (paxcounterModule->deviceListSent)
    {
        LOG_DEBUG("PaxcounterModule: Device list already sent, skipping");
        return false;
    }

    LOG_DEBUG("PaxcounterModule: About to call libpax_list");

    // Log the device list structure before calling libpax_list
    LOG_DEBUG("PaxcounterModule: Before libpax_list - count: %d, capacity: %d, devices ptr: %p",
              devices_from_libpax.count, devices_from_libpax.capacity, devices_from_libpax.devices);

    int result = libpax_list(&devices_from_libpax);

    LOG_DEBUG("PaxcounterModule: libpax_list returned: %d", result);

    // Log the device list structure after calling libpax_list
    LOG_DEBUG("PaxcounterModule: After libpax_list - count: %d, capacity: %d, devices ptr: %p",
              devices_from_libpax.count, devices_from_libpax.capacity, devices_from_libpax.devices);

    if (result != 0)
    {
        LOG_ERROR("PaxcounterModule: libpax_list failed with code: %d", result);
        return false;
    }

    if (devices_from_libpax.count == 0)
    {
        LOG_DEBUG("PaxcounterModule: No devices to send, count is 0");
        paxcounterModule->deviceListSent = true;
        return false;
    }

    if (devices_from_libpax.devices == nullptr)
    {
        LOG_ERROR("PaxcounterModule: Device list pointer is null!");
        return false;
    }

    LOG_INFO("PaxcounterModule: send device list with %d devices", devices_from_libpax.count);

    // Log first few devices for debugging
    for (uint32_t i = 0; i < min(devices_from_libpax.count, (uint32_t)3); i++)
    {
        if (i < devices_from_libpax.capacity)
        {
            LOG_DEBUG("PaxcounterModule: Device[%d] - type: %d, rssi: %d, timestamp: %lu, mac ptr: %p",
                      i, devices_from_libpax.devices[i].type, devices_from_libpax.devices[i].rssi,
                      devices_from_libpax.devices[i].timestamp, devices_from_libpax.devices[i].mac);
        }
    }

    // Create a PaxList message
    LOG_DEBUG("PaxcounterModule: Creating PaxList message");
    meshtastic_PaxList pl = meshtastic_PaxList_init_default;
    pl.count = devices_from_libpax.count;
    pl.capacity = devices_from_libpax.capacity;

    // Set up the callback for handling the devices array
    LOG_DEBUG("PaxcounterModule: Setting up encode callback");
    pl.devices.funcs.encode = [](pb_ostream_t* stream, const pb_field_t* field, void* const * arg) -> bool
    {
        LOG_DEBUG("PaxcounterModule: In encode callback, arg: %p", arg);

        if (!arg || !*arg)
        {
            LOG_ERROR("PaxcounterModule: Null arg pointer in encode callback");
            return false;
        }

        pax_device_list_t* deviceList = (pax_device_list_t*)*arg;

        LOG_DEBUG("PaxcounterModule: DeviceList in callback - count: %d, capacity: %d, devices ptr: %p",
                  deviceList->count, deviceList->capacity, deviceList->devices);

        if (!deviceList->devices)
        {
            LOG_ERROR("PaxcounterModule: Null devices pointer in encode callback");
            return false;
        }

        // Limit number of devices to encode to avoid buffer overflow
        uint32_t device_count = min(deviceList->count, (uint32_t)10);
        LOG_DEBUG("PaxcounterModule: Will encode %d devices", device_count);

        // Iterate through each device in the list
        for (uint32_t i = 0; i < device_count; i++)
        {
            LOG_DEBUG("PaxcounterModule: Processing device %d", i);

            // Safety check for device index
            if (i >= deviceList->capacity)
            {
                LOG_ERROR("PaxcounterModule: Device index out of bounds: %d >= %d", i, deviceList->capacity);
                return false;
            }

            // Start a submessage for each device
            if (!pb_encode_tag_for_field(stream, field))
            {
                LOG_ERROR("PaxcounterModule: Failed to encode tag for device %d", i);
                return false;
            }

            // Get the current device info
            pax_device_info_t* device = &deviceList->devices[i];

            LOG_DEBUG("PaxcounterModule: Device[%d] - type: %d, rssi: %d, mac ptr: %p",
                      i, device->type, device->rssi, device->mac);

            if (!device->mac)
            {
                LOG_ERROR("PaxcounterModule: Null MAC address for device %d", i);
                return false;
            }

            // Create and populate a PaxDevice message
            meshtastic_PaxDevice paxDevice = meshtastic_PaxDevice_init_default;

            switch (device->type)
            {
            case MAC_SNIFF_WIFI:
                paxDevice.device_type = meshtastic_DeviceType_DEVICE_TYPE_WIFI;
                LOG_DEBUG("PaxcounterModule: Device %d is WIFI", i);
                break;
            case MAC_SNIFF_BLE_ENS:
                LOG_DEBUG("PaxcounterModule: Device %d is BLE_ENS", i);
                paxDevice.device_type = meshtastic_DeviceType_DEVICE_TYPE_BLE;
                break;
            case MAC_SNIFF_BLE:
                LOG_DEBUG("PaxcounterModule: Device %d is BLE", i);
                paxDevice.device_type = meshtastic_DeviceType_DEVICE_TYPE_BLE;
                break;
            default:
                LOG_DEBUG("PaxcounterModule: Device %d has unknown type: %d", i, device->type);
                paxDevice.device_type = meshtastic_DeviceType_DEVICE_TYPE_BLE;
                break;
            }

            paxDevice.rssi = device->rssi;
            paxDevice.timestamp = device->timestamp;

            LOG_DEBUG("PaxcounterModule: Setting up MAC address callback for device %d", i);
            paxDevice.mac_address.funcs.encode = [](pb_ostream_t* stream, const pb_field_t* field,
                                                    void* const * arg) -> bool
            {
                LOG_DEBUG("PaxcounterModule: In MAC address encode callback, arg: %p", arg);

                if (!arg || !*arg)
                {
                    LOG_ERROR("PaxcounterModule: Null MAC address arg");
                    return false;
                }

                uint8_t* mac = (uint8_t*)*arg;

                LOG_DEBUG("PaxcounterModule: MAC bytes: %02x:%02x:%02x:%02x:%02x:%02x",
                          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

                if (!pb_encode_tag_for_field(stream, field))
                {
                    LOG_ERROR("PaxcounterModule: Failed to encode tag for MAC field");
                    return false;
                }

                bool result = pb_encode_string(stream, mac, 6);
                LOG_DEBUG("PaxcounterModule: MAC encoding result: %d", result);
                return result;
            };

            paxDevice.mac_address.arg = device->mac;

            // Encode the device as a submessage
            LOG_DEBUG("PaxcounterModule: Encoding device %d as submessage", i);
            if (!pb_encode_submessage(stream, meshtastic_PaxDevice_fields, &paxDevice))
            {
                LOG_ERROR("PaxcounterModule: Failed to encode device %d as submessage", i);
                return false;
            }

            LOG_DEBUG("PaxcounterModule: Successfully encoded device %d", i);
        }

        LOG_DEBUG("PaxcounterModule: Completed device encoding, returning true");
        return true;
    };

    pl.devices.arg = &devices_from_libpax;
    LOG_DEBUG("PaxcounterModule: Set device list argument: %p", &devices_from_libpax);

    // Allocate packet
    LOG_DEBUG("PaxcounterModule: Allocating data packet");
    meshtastic_MeshPacket* p = allocDataPacket();
    p->to = dest;
    p->decoded.want_response = false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;

    // Manually encode the protobuf
    LOG_DEBUG("PaxcounterModule: About to encode protobuf");
    size_t written_size = 0;

    try
    {
        written_size = pb_encode_to_bytes(p->decoded.payload.bytes,
                                          sizeof(p->decoded.payload.bytes),
                                          meshtastic_PaxList_fields,
                                          &pl);

        LOG_DEBUG("PaxcounterModule: Protobuf encoding completed, size: %d", (int)written_size);

        if (written_size == 0)
        {
            LOG_ERROR("PaxcounterModule: Failed to encode protobuf - zero size returned");
            packetPool.release(p);
            return false;
        }

        p->decoded.payload.size = written_size;
    }
    catch (...)
    {
        LOG_ERROR("PaxcounterModule: Exception during protobuf encoding!");
        packetPool.release(p);
        return false;
    }

    // Use a different port number for PaxList messages
    LOG_DEBUG("PaxcounterModule: Setting port number");
    p->decoded.portnum = meshtastic_PortNum_PAXCOUNTER_LIST_APP; // Use a distinct port

    LOG_DEBUG("PaxcounterModule: About to send to mesh");
    service->sendToMesh(p, RX_SRC_LOCAL, true);

    LOG_DEBUG("PaxcounterModule: Successfully sent to mesh");
    paxcounterModule->deviceListSent = true;

    return true;
}

bool PaxcounterModule::handleReceivedProtobuf(const meshtastic_MeshPacket& mp, meshtastic_Paxcount* p)
{
    return false; // Let others look at this message also if they want. We don't do anything with received packets.
}

meshtastic_MeshPacket* PaxcounterModule::allocReply()
{
    meshtastic_Paxcount pl = meshtastic_Paxcount_init_default;
    pl.wifi = count_from_libpax.wifi_count;
    pl.ble = count_from_libpax.ble_count;
    pl.uptime = millis() / 1000;
    return allocDataProtobuf(pl);
}

// Add this at the beginning of your runOnce() method in PaxcounterModule.cpp:
int32_t PaxcounterModule::runOnce()
{
    if (isActive())
    {
        LOG_DEBUG("PaxcounterModule::runOnce - isActive is true");

        if (firstTime)
        {
            // Log existing code
            LOG_DEBUG("PaxcounterModule: Before firstTime initialization");

            firstTime = false;
            LOG_DEBUG("Paxcounter starting up with interval of %d seconds",
                      Default::getConfiguredOrDefault(moduleConfig.paxcounter.paxcounter_update_interval,
                          default_telemetry_broadcast_interval_secs));
            struct libpax_config_t configuration;
            libpax_default_config(&configuration);

            configuration.blecounter = 1;
            configuration.blescantime = 0; // infinite
            configuration.wificounter = 1;
            configuration.wifi_channel_map = WIFI_CHANNEL_ALL;
            configuration.wifi_channel_switch_interval = 50;
            configuration.wifi_rssi_threshold = Default::getConfiguredOrDefault(
                moduleConfig.paxcounter.wifi_threshold, -80);
            configuration.ble_rssi_threshold = Default::getConfiguredOrDefault(
                moduleConfig.paxcounter.ble_threshold, -80);
            libpax_update_config(&configuration);

            // internal processing initialization
            libpax_init(handlePaxCounterReportRequest, &count_from_libpax, &devices_from_libpax,
                        Default::getConfiguredOrDefault(moduleConfig.paxcounter.paxcounter_update_interval,
                                                        default_telemetry_broadcast_interval_secs),
                        0);
            libpax_start();

            // Rest of your code...

            LOG_DEBUG("PaxcounterModule: After libpax_init and libpax_start calls");
        }
        else
        {
            LOG_DEBUG("PaxcounterModule: In runOnce, about to send data");

            // First send the count information - add debug before and after
            LOG_DEBUG("PaxcounterModule: Before calling sendInfo()");
            bool infoSent = sendInfo(NODENUM_BROADCAST);
            LOG_DEBUG("PaxcounterModule: sendInfo() returned: %d", infoSent);

            // Then send the device list - add debug before and after
            LOG_DEBUG("PaxcounterModule: Before calling sendDeviceList()");
            bool deviceListSent = sendDeviceList(NODENUM_BROADCAST);
            LOG_DEBUG("PaxcounterModule: sendDeviceList() returned: %d", deviceListSent);

            LOG_DEBUG("PaxcounterModule: Completed sending data in runOnce");
        }

        // Return the interval...
        LOG_DEBUG("PaxcounterModule: Calculating next interval");
        return Default::getConfiguredOrDefaultMsScaled(moduleConfig.paxcounter.paxcounter_update_interval,
                                                       default_telemetry_broadcast_interval_secs, numOnlineNodes);
    }
    else
    {
        LOG_DEBUG("PaxcounterModule::runOnce - isActive is false, disabling");
        return disable();
    }
}

#if HAS_SCREEN

#include "graphics/ScreenFonts.h"

void PaxcounterModule::drawFrame(OLEDDisplay* display, OLEDDisplayUiState* state, int16_t x, int16_t y)
{
    char buffer[50];
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    display->setFont(FONT_SMALL);
    display->drawString(x + 0, y + 0, "PAX");

    libpax_count(&count_from_libpax);

    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->setFont(FONT_SMALL);
    display->drawStringf(display->getWidth() / 2 + x, 0 + y + 12, buffer, "WiFi: %d\nBLE: %d\nuptime: %ds",
                         count_from_libpax.wifi_count, count_from_libpax.ble_count, millis() / 1000);
}
#endif // HAS_SCREEN

#endif
