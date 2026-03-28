#include "xbox_led.h"

#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define GIP_REENUMERATE 0x40001CD0

#pragma pack(push, 1)
typedef struct {
    uint64_t deviceId;
    uint8_t  commandId;
    uint8_t  clientFlags;
    uint8_t  sequence;
    uint8_t  unknown1;
    uint32_t length;
    uint32_t unknown2;
} GipHeader;
#pragma pack(pop)

void xbox_init(XboxController *ctrl)
{
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->seq = 1;
}

static bool has_device_id(const uint64_t *device_ids, int count, uint64_t device_id)
{
    for (int i = 0; i < count; i++) {
        if (device_ids[i] == device_id)
            return true;
    }
    return false;
}

static int discover_devices(HANDLE h, HANDLE read_event, uint64_t *device_ids, int max_devices)
{
    if (!device_ids || max_devices <= 0)
        return 0;

    DWORD bytes = 0;
    DeviceIoControl(h, GIP_REENUMERATE, NULL, 0, NULL, 0, &bytes, NULL);

    uint8_t buf[4096];
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = read_event;

    int found = 0;

    for (int i = 0; i < 16; i++) {
        memset(buf, 0, sizeof(buf));
        ResetEvent(ov.hEvent);
        DWORD rd = 0;
        BOOL ok = ReadFile(h, buf, sizeof(buf), &rd, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            DWORD wait = WaitForSingleObject(ov.hEvent, 300);
            if (wait == WAIT_TIMEOUT) {
                CancelIo(h);
                WaitForSingleObject(ov.hEvent, 100);
                continue;
            }
            GetOverlappedResult(h, &ov, &rd, FALSE);
        }
        if (rd < sizeof(GipHeader))
            continue;

        GipHeader *hdr = (GipHeader *)buf;
        if (hdr->commandId == 0x01 || hdr->commandId == 0x02) {
            if (!has_device_id(device_ids, found, hdr->deviceId)) {
                if (found < max_devices) {
                    device_ids[found++] = hdr->deviceId;
                } else {
                    break;
                }
            }
        }
    }

    return found;
}

bool xbox_enumerate_devices(uint64_t *device_ids, int max_devices, int *out_count)
{
    if (out_count)
        *out_count = 0;
    if (!device_ids || max_devices <= 0)
        return false;

    HANDLE h = CreateFileW(L"\\\\.\\XboxGIP",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        NULL);

    if (h == INVALID_HANDLE_VALUE)
        return false;

    HANDLE read_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!read_event) {
        CloseHandle(h);
        return false;
    }

    int found = discover_devices(h, read_event, device_ids, max_devices);

    CloseHandle(read_event);
    CloseHandle(h);

    if (out_count)
        *out_count = found;
    return found > 0;
}

bool xbox_open(XboxController *ctrl)
{
    return xbox_open_device(ctrl, 0);
}

bool xbox_open_device(XboxController *ctrl, uint64_t device_id)
{
    xbox_close(ctrl);

    HANDLE h = CreateFileW(L"\\\\.\\XboxGIP",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        NULL);

    if (h == INVALID_HANDLE_VALUE) {
        snprintf(ctrl->error, sizeof(ctrl->error),
                 "Cannot open XboxGIP driver (error %lu)", GetLastError());
        ctrl->last_err = XBOX_ERR_OPEN_FAILED;
        return false;
    }
    ctrl->handle = h;
    ctrl->read_event = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (!ctrl->read_event) {
        snprintf(ctrl->error, sizeof(ctrl->error), "Cannot create read event");
        ctrl->last_err = XBOX_ERR_OPEN_FAILED;
        xbox_close(ctrl);
        return false;
    }

    uint64_t ids[16];
    int count = discover_devices((HANDLE)ctrl->handle, (HANDLE)ctrl->read_event, ids, 16);

    if (count <= 0) {
        snprintf(ctrl->error, sizeof(ctrl->error), "No Xbox controller found");
        ctrl->last_err = XBOX_ERR_NO_DEVICE;
        xbox_close(ctrl);
        return false;
    }

    uint64_t target_id = device_id;
    if (target_id == 0)
        target_id = ids[0];

    if (!has_device_id(ids, count, target_id)) {
        snprintf(ctrl->error, sizeof(ctrl->error), "Selected controller not found");
        ctrl->last_err = XBOX_ERR_NO_DEVICE;
        xbox_close(ctrl);
        return false;
    }

    ctrl->device_id = target_id;
    ctrl->connected = true;
    ctrl->last_err = XBOX_OK;
    ctrl->error[0] = '\0';
    return true;
}

void xbox_close(XboxController *ctrl)
{
    if (ctrl->read_event) {
        CloseHandle((HANDLE)ctrl->read_event);
        ctrl->read_event = NULL;
    }
    if (ctrl->handle) {
        CloseHandle((HANDLE)ctrl->handle);
        ctrl->handle = NULL;
    }
    ctrl->device_id = 0;
    ctrl->connected = false;
}

void xbox_cleanup(XboxController *ctrl)
{
    xbox_close(ctrl);
}

bool xbox_set_led(XboxController *ctrl, uint8_t mode, uint8_t brightness)
{
    if (!ctrl->connected || !ctrl->handle)
        return false;

    if (brightness > LED_BRIGHTNESS_MAX)
        brightness = LED_BRIGHTNESS_MAX;

    uint8_t payload[] = { 0x00, mode, brightness };
    uint8_t pkt[sizeof(GipHeader) + sizeof(payload)];
    memset(pkt, 0, sizeof(pkt));

    GipHeader *hdr = (GipHeader *)pkt;
    hdr->deviceId = ctrl->device_id;
    hdr->commandId = GIP_CMD_LED;
    hdr->clientFlags = GIP_OPT_INTERNAL;
    hdr->sequence = ctrl->seq;
    hdr->length = sizeof(payload);
    memcpy(pkt + sizeof(GipHeader), payload, sizeof(payload));

    ctrl->seq = (ctrl->seq % 255) + 1;

    DWORD written = 0;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    BOOL ok = WriteFile((HANDLE)ctrl->handle, pkt, sizeof(pkt), &written, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        WaitForSingleObject(ov.hEvent, 2000);
        GetOverlappedResult((HANDLE)ctrl->handle, &ov, &written, FALSE);
    }
    CloseHandle(ov.hEvent);

    if (written != sizeof(pkt)) {
        snprintf(ctrl->error, sizeof(ctrl->error),
                 "Write failed (error %lu)", GetLastError());
        ctrl->last_err = XBOX_ERR_SEND;
        return false;
    }

    return true;
}

bool xbox_set_brightness(XboxController *ctrl, uint8_t brightness)
{
    if (brightness == 0)
        return xbox_set_led(ctrl, LED_MODE_OFF, 0);
    return xbox_set_led(ctrl, LED_MODE_ON, brightness);
}

bool xbox_led_off(XboxController *ctrl)
{
    return xbox_set_led(ctrl, LED_MODE_OFF, 0);
}
