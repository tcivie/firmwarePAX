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
    // The libpax library already updated our data structure, just before invoking this callback.
    LOG_INFO("PaxcounterModule: libpax reported new data: wifi=%d; ble=%d; uptime=%lu",
             paxcounterModule->count_from_libpax.wifi_count, paxcounterModule->count_from_libpax.ble_count,
             millis() / 1000);
    paxcounterModule->reportedDataSent = false;
    paxcounterModule->deviceListSent = false;
    paxcounterModule->setIntervalFromNow(0);
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
    if (paxcounterModule->deviceListSent)
        return false;

    libpax_list(&devices_from_libpax);

    LOG_INFO("PaxcounterModule: send device list with %d devices", devices_from_libpax.count);

    // Create a PaxList message
    meshtastic_PaxList pl = meshtastic_PaxList_init_default;
    pl.count = devices_from_libpax.count;
    pl.capacity = devices_from_libpax.capacity;

    // Set up the callback for handling the devices array
    pl.devices.funcs.encode = [](pb_ostream_t* stream, const pb_field_t* field, void* const * arg) -> bool
    {
        pax_device_list_t* deviceList = (pax_device_list_t*)*arg;

        // Iterate through each device in the list
        for (uint32_t i = 0; i < deviceList->count; i++)
        {
            // Start a submessage for each device
            if (!pb_encode_tag_for_field(stream, field))
                return false;

            // Get the current device info
            pax_device_info_t* device = &deviceList->devices[i];

            // Create and populate a PaxDevice message
            meshtastic_PaxDevice paxDevice = meshtastic_PaxDevice_init_default;

            switch (device->type)
            {
            case MAC_SNIFF_WIFI:
                paxDevice.device_type = meshtastic_DeviceType_DEVICE_TYPE_WIFI;
                break;
            case MAC_SNIFF_BLE_ENS:
            case MAC_SNIFF_BLE:
            default:
                paxDevice.device_type = meshtastic_DeviceType_DEVICE_TYPE_BLE;
                break;
            }
            paxDevice.rssi = device->rssi;
            paxDevice.timestamp = device->timestamp;
            paxDevice.mac_address.funcs.encode = [](pb_ostream_t* stream, const pb_field_t* field,
                                                    void* const * arg) -> bool
            {
                uint8_t* mac = (uint8_t*)*arg;
                if (!pb_encode_tag_for_field(stream, field))
                    return false;
                return pb_encode_string(stream, mac, 6); // MAC address is 6 bytes
            };
            paxDevice.mac_address.arg = device->mac;

            // Encode the device as a submessage
            if (!pb_encode_submessage(stream, meshtastic_PaxDevice_fields, &paxDevice))
                return false;
        }

        return true;
    };
    pl.devices.arg = &devices_from_libpax;

    // We need to manually allocate and encode the packet since we're not using the templated method
    meshtastic_MeshPacket* p = allocDataPacket();
    p->to = dest;
    p->decoded.want_response = false;
    p->priority = meshtastic_MeshPacket_Priority_BACKGROUND;

    // Manually encode the protobuf
    p->decoded.payload.size = pb_encode_to_bytes(p->decoded.payload.bytes,
                                                 sizeof(p->decoded.payload.bytes),
                                                 meshtastic_PaxList_fields,
                                                 &pl);

    // Use a different port number for PaxList messages
    p->decoded.portnum = meshtastic_PortNum_PAXCOUNTER_LIST_APP;

    service->sendToMesh(p, RX_SRC_LOCAL, true);

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

int32_t PaxcounterModule::runOnce()
{
    if (isActive())
    {
        if (firstTime)
        {
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
        }
        else
        {
            // First send the count information
            sendInfo(NODENUM_BROADCAST);

            // Then send the device list
            sendDeviceList(NODENUM_BROADCAST);
        }
        return Default::getConfiguredOrDefaultMsScaled(moduleConfig.paxcounter.paxcounter_update_interval,
                                                       default_telemetry_broadcast_interval_secs, numOnlineNodes);
    }
    else
    {
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
