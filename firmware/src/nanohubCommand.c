/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <plat/inc/taggedPtr.h>
#include <plat/inc/rtc.h>
#include <inttypes.h>
#include <string.h>
#include <stdint.h>
#include <sys/endian.h>

#include <atomicBitset.h>
#include <hostIntf.h>
#include <hostIntf_priv.h>
#include <nanohubCommand.h>
#include <nanohubPacket.h>
#include <eeData.h>
#include <seos.h>
#include <util.h>
#include <mpu.h>
#include <heap.h>
#include <sensType.h>
#include <timer.h>
#include <crc.h>
#include <rsa.h>
#include <appSec.h>
#include <plat/inc/bl.h>
#include <plat/inc/plat.h>
#include <variant/inc/variant.h>

#define NANOHUB_COMMAND(_reason, _handler, _minReqType, _maxReqType) \
        { .reason = _reason, .handler = _handler, \
          .minDataLen = sizeof(_minReqType), .maxDataLen = sizeof(_maxReqType) }

#define NANOHUB_HAL_COMMAND(_msg, _handler) \
        { .msg = _msg, .handler = _handler }

static struct DownloadState
{
    struct AppSecState *appSecState;
    uint32_t size;
    uint32_t srcOffset;
    uint32_t dstOffset;
    uint8_t *start;
    uint32_t crc;
    uint32_t srcCrc;
    uint8_t  data[NANOHUB_PACKET_PAYLOAD_MAX];
    uint8_t  len;
    uint8_t  chunkReply;
    uint8_t  type;
    bool     erase;
} *mDownloadState;

static AppSecErr mAppSecStatus;

static uint32_t getOsHwVersion(void *rx, uint8_t rx_len, void *tx, uint64_t timestamp)
{
    struct NanohubOsHwVersionsResponse *resp = tx;
    resp->hwType = htole16(platHwType());
    resp->hwVer = htole16(platHwVer());
    resp->blVer = htole16(platBlVer());
    resp->osVer = htole16(OS_VER);
    resp->variantVer = htole32(VARIANT_VER);

    return sizeof(*resp);
}

static uint32_t getAppVersion(void *rx, uint8_t rx_len, void *tx, uint64_t timestamp)
{
    struct NanohubAppVersionsRequest *req = rx;
    struct NanohubAppVersionsResponse *resp = tx;
    uint32_t appIdx, appVer, appSize;

    if (osAppInfoById(le64toh(req->appId), &appIdx, &appVer, &appSize)) {
        resp->appVer = htole32(appVer);
        return sizeof(*resp);
    }

    return 0;
}

static uint32_t queryAppInfo(void *rx, uint8_t rx_len, void *tx, uint64_t timestamp)
{
    struct NanohubAppInfoRequest *req = rx;
    struct NanohubAppInfoResponse *resp = tx;
    uint64_t appId;
    uint32_t appVer, appSize;

    if (osAppInfoByIndex(le32toh(req->appIdx), &appId, &appVer, &appSize)) {
        resp->appId = htole64(appId);
        resp->appVer = htole32(appVer);
        resp->appSize = htole32(appSize);
        return sizeof(*resp);
    }

    return 0;
}

static AppSecErr writeCbk(const void *data, uint32_t len)
{
    AppSecErr ret;

    mpuAllowRamExecution(true);
    mpuAllowRomWrite(true);
    if (!BL.blProgramShared(mDownloadState->start + mDownloadState->dstOffset, (uint8_t *)data, len, BL_FLASH_KEY1, BL_FLASH_KEY2)) {
        ret = APP_SEC_BAD;
    } else {
        ret = APP_SEC_NO_ERROR;
        mDownloadState->dstOffset += len;
    }
    mpuAllowRomWrite(false);
    mpuAllowRamExecution(false);

    return ret;
}

static AppSecErr pubKeyFindCbk(const uint32_t *gotKey, bool *foundP)
{
    const uint32_t *ptr;
    uint32_t numKeys, i;

    *foundP = false;
    ptr = BL.blGetPubKeysInfo(&numKeys);
    for (i = 0; ptr && i < numKeys; i++, ptr += RSA_LIMBS) {
        if (!memcmp(gotKey, ptr, RSA_BYTES)) {
            *foundP = true;
            break;
        }
    }

    return APP_SEC_NO_ERROR;
}

static AppSecErr aesKeyAccessCbk(uint64_t keyIdx, void *keyBuf)
{
    struct SeosEedataEncrKeyData kd;
    void *state = NULL;

    while(1) {
        uint32_t sz = sizeof(struct SeosEedataEncrKeyData);

        if (!eeDataGetAllVersions(EE_DATA_NAME_ENCR_KEY, &kd, &sz, &state))
            break;

        if (sz == sizeof(struct SeosEedataEncrKeyData) && kd.keyID == keyIdx) {
            memcpy(keyBuf, kd.key, sizeof(kd.key));
            return APP_SEC_NO_ERROR;
        }
    }

    return APP_SEC_KEY_NOT_FOUND;
}

static void resetDownloadState()
{
    mAppSecStatus = APP_SEC_NO_ERROR;
    if (mDownloadState->appSecState)
        appSecDeinit(mDownloadState->appSecState);
    mDownloadState->appSecState = appSecInit(writeCbk, pubKeyFindCbk, aesKeyAccessCbk, true);
    mDownloadState->srcOffset = 0;
    mDownloadState->srcCrc = ~0;
    mDownloadState->dstOffset = 4; // skip over header
}

static uint32_t startFirmwareUpload(void *rx, uint8_t rx_len, void *tx, uint64_t timestamp)
{
    struct NanohubStartFirmwareUploadRequest *req = rx;
    struct NanohubStartFirmwareUploadResponse *resp = tx;
    uint8_t *shared, *shared_start, *shared_end;
    int len, total_len;
    uint32_t sharedSz;

    shared_start = platGetSharedAreaInfo(&sharedSz);
    shared_end = shared_start + sharedSz;

    if (!mDownloadState) {
        mDownloadState = heapAlloc(sizeof(struct DownloadState));

        if (!mDownloadState) {
            resp->accepted = false;
            return sizeof(*resp);
        } else {
            memset(mDownloadState, 0x00, sizeof(struct DownloadState));
        }
    }

    mDownloadState->type = req->type;
    mDownloadState->size = le32toh(req->size);
    mDownloadState->crc = le32toh(req->crc);
    mDownloadState->chunkReply = NANOHUB_FIRMWARE_CHUNK_REPLY_ACCEPTED;

    for (shared = shared_start;
         shared < shared_end && shared[0] != 0xFF;
         shared += total_len) {
        len = (shared[1] << 16) | (shared[2] << 8) | shared[3];
        total_len = sizeof(uint32_t) + ((len + 3) & ~3) + sizeof(uint32_t);
    }

    if (shared + sizeof(uint32_t) + ((mDownloadState->size + 3) & ~3) + sizeof(uint32_t) < shared_end) {
        mDownloadState->start = shared;
        mDownloadState->erase = false;
    } else {
        mDownloadState->start = shared_start;
        mDownloadState->erase = true;
    }
    resetDownloadState();

    resp->accepted = true;
    return sizeof(*resp);
}

static void firmwareErase(void *cookie)
{
    osLog(LOG_INFO, "hostIntfFirmwareErase: Firmware Erase\n");
    if (mDownloadState->erase == true) {
        mpuAllowRamExecution(true);
        mpuAllowRomWrite(true);
        (void)BL.blEraseShared(BL_FLASH_KEY1, BL_FLASH_KEY2);
        mpuAllowRomWrite(false);
        mpuAllowRamExecution(false);
        mDownloadState->erase = false;
        hostIntfSetInterrupt(NANOHUB_INT_CMD_WAIT);
    }
}

static AppSecErr giveAppSecTimeIfNeeded(struct AppSecState *state, AppSecErr prevRet)
{
    /* XXX: this will need to go away for real asynchronicity */

    while (prevRet == APP_SEC_NEED_MORE_TIME)
        prevRet = appSecDoSomeProcessing(state);

    return prevRet;
}

static uint8_t firmwareFinish(bool valid)
{
    uint8_t buffer[7];
    int padlen;
    uint32_t crc;
    uint16_t marker;
    static const char magic[] = APP_HDR_MAGIC;
    const struct AppHdr *app;

    mAppSecStatus = appSecRxDataOver(mDownloadState->appSecState);
    mAppSecStatus = giveAppSecTimeIfNeeded(mDownloadState->appSecState, mAppSecStatus);

    if (mAppSecStatus == APP_SEC_NO_ERROR && valid && mDownloadState->type == BL_FLASH_APP_ID) {
        app = (const struct AppHdr *)&mDownloadState->start[4];

        if (app->marker != APP_HDR_MARKER_UPLOADING ||
                mDownloadState->size < sizeof(uint32_t) + sizeof(struct AppHdr) ||
                memcmp(magic, app->magic, sizeof(magic)-1) ||
                app->fmtVer != APP_HDR_VER_CUR) {
            marker = APP_HDR_MARKER_DELETED;
        } else {
            marker = APP_HDR_MARKER_VALID;
        }

        osLog(LOG_INFO, "Loaded %s app: %ld bytes @ %p\n", marker == APP_HDR_MARKER_VALID ? "valid" : "invalid", mDownloadState->size, mDownloadState->start);

        mpuAllowRamExecution(true);
        mpuAllowRomWrite(true);
        if (!BL.blProgramShared((uint8_t *)&app->marker, (uint8_t *)&marker, sizeof(marker), BL_FLASH_KEY1, BL_FLASH_KEY2)) {
            mpuAllowRomWrite(false);
            mpuAllowRamExecution(false);
            return NANOHUB_FIRMWARE_CHUNK_REPLY_RESEND;
        }
    } else {
        mpuAllowRamExecution(true);
        mpuAllowRomWrite(true);
    }

    if (mAppSecStatus == APP_SEC_NO_ERROR && valid)
        buffer[0] = ((mDownloadState->type & 0xF) << 4) | (mDownloadState->type & 0xF);
    else
        buffer[0] = 0x00;

    buffer[1] = (mDownloadState->dstOffset - 4) >> 16;
    buffer[2] = (mDownloadState->dstOffset - 4) >> 8;
    buffer[3] = (mDownloadState->dstOffset - 4);

    if (!BL.blProgramShared(mDownloadState->start, buffer, 4, BL_FLASH_KEY1, BL_FLASH_KEY2)) {
        mpuAllowRomWrite(false);
        mpuAllowRamExecution(false);
        return NANOHUB_FIRMWARE_CHUNK_REPLY_RESEND;
    }

    crc = ~crc32(mDownloadState->start, mDownloadState->dstOffset, ~0);
    padlen =  (4 - (mDownloadState->dstOffset & 3)) & 3;
    memset(buffer, 0x00, padlen);
    memcpy(&buffer[padlen], &crc, sizeof(uint32_t));
    mDownloadState->size = mDownloadState->dstOffset + padlen + sizeof(uint32_t);

    if (!BL.blProgramShared(mDownloadState->start + mDownloadState->dstOffset, buffer, padlen + sizeof(uint32_t), BL_FLASH_KEY1, BL_FLASH_KEY2)) {
        mpuAllowRomWrite(false);
        mpuAllowRamExecution(false);
        return NANOHUB_FIRMWARE_CHUNK_REPLY_RESEND;
    }

    mpuAllowRomWrite(false);
    mpuAllowRamExecution(false);
    heapFree(mDownloadState);
    mDownloadState = NULL;

    return NANOHUB_FIRMWARE_CHUNK_REPLY_ACCEPTED;
}

static void firmwareWrite(void *cookie)
{
    uint32_t reply;

    if (mDownloadState->type == BL_FLASH_APP_ID) {
        /* XXX: this will need to change for real asynchronicity */
        const uint8_t *data = mDownloadState->data;
        uint32_t len = mDownloadState->len, lenLeft;
        mAppSecStatus = APP_SEC_NO_ERROR;

        while (len) {
            mAppSecStatus = appSecRxData(mDownloadState->appSecState, data, len, &lenLeft);
            data += len - lenLeft;
            len = lenLeft;

            mAppSecStatus = giveAppSecTimeIfNeeded(mDownloadState->appSecState, mAppSecStatus);
        }
    }
    else
        mAppSecStatus = writeCbk(mDownloadState->data, mDownloadState->len);

    if (mAppSecStatus == APP_SEC_NO_ERROR) {
        if (mDownloadState->srcOffset == mDownloadState->size && mDownloadState->crc == ~mDownloadState->srcCrc) {
            reply = firmwareFinish(true);
            if (mDownloadState)
                mDownloadState->chunkReply = reply;
        } else {
            mDownloadState->chunkReply = NANOHUB_FIRMWARE_CHUNK_REPLY_ACCEPTED;
        }
    } else {
        heapFree(mDownloadState);
        mDownloadState = NULL;
    }
    hostIntfSetBusy(false);
}

static uint32_t firmwareChunk(void *rx, uint8_t rx_len, void *tx, uint64_t timestamp)
{
    uint32_t offset;
    uint8_t len;
    struct NanohubFirmwareChunkRequest *req = rx;
    struct NanohubFirmwareChunkResponse *resp = tx;

    offset = le32toh(req->offset);
    len = rx_len - sizeof(req->offset);

    if (!mDownloadState) {
        resp->chunkReply = NANOHUB_FIRMWARE_CHUNK_REPLY_CANCEL_NO_RETRY;
    } else if (mDownloadState->chunkReply != NANOHUB_FIRMWARE_CHUNK_REPLY_ACCEPTED) {
        resp->chunkReply = mDownloadState->chunkReply;
        heapFree(mDownloadState);
        mDownloadState = NULL;
    } else if (mDownloadState->erase == true) {
        resp->chunkReply = NANOHUB_FIRMWARE_CHUNK_REPLY_WAIT;
        osDefer(firmwareErase, NULL, false);
    } else if (offset != mDownloadState->srcOffset) {
        resp->chunkReply = NANOHUB_FIRMWARE_CHUNK_REPLY_RESTART;
        resetDownloadState();
    } else {
        mDownloadState->srcCrc = crc32(req->data, len, mDownloadState->srcCrc);
        mDownloadState->srcOffset += len;
        if ((mDownloadState->srcOffset == mDownloadState->size && mDownloadState->crc != ~mDownloadState->srcCrc) || (mDownloadState->srcOffset > mDownloadState->size)) {
            firmwareFinish(false);
            resp->chunkReply = NANOHUB_FIRMWARE_CHUNK_REPLY_CANCEL;
        } else {
            memcpy(mDownloadState->data, req->data, len);
            mDownloadState->len = len;
            resp->chunkReply = NANOHUB_FIRMWARE_CHUNK_REPLY_ACCEPTED;
            hostIntfSetBusy(true);
            osDefer(firmwareWrite, NULL, false);
        }
    }

    return sizeof(*resp);
}

static uint32_t finishFirmwareUpload(void *rx, uint8_t rx_len, void *tx, uint64_t timestamp)
{
    struct NanohubFinishFirmwareUploadResponse *resp = tx;

    if (!mDownloadState) {
        switch (mAppSecStatus) {
        case APP_SEC_NO_ERROR:
            resp->uploadReply = NANOHUB_FIRMWARE_UPLOAD_SUCCESS;
            break;
        case APP_SEC_KEY_NOT_FOUND:
            resp->uploadReply = NANOHUB_FIRMWARE_UPLOAD_APP_SEC_KEY_NOT_FOUND;
            break;
        case APP_SEC_HEADER_ERROR:
            resp->uploadReply = NANOHUB_FIRMWARE_UPLOAD_APP_SEC_HEADER_ERROR;
            break;
        case APP_SEC_TOO_MUCH_DATA:
            resp->uploadReply = NANOHUB_FIRMWARE_UPLOAD_APP_SEC_TOO_MUCH_DATA;
            break;
        case APP_SEC_TOO_LITTLE_DATA:
            resp->uploadReply = NANOHUB_FIRMWARE_UPLOAD_APP_SEC_TOO_LITTLE_DATA;
            break;
        case APP_SEC_SIG_VERIFY_FAIL:
            resp->uploadReply = NANOHUB_FIRMWARE_UPLOAD_APP_SEC_SIG_VERIFY_FAIL;
            break;
        case APP_SEC_SIG_DECODE_FAIL:
            resp->uploadReply = NANOHUB_FIRMWARE_UPLOAD_APP_SEC_SIG_DECODE_FAIL;
            break;
        case APP_SEC_SIG_ROOT_UNKNOWN:
            resp->uploadReply = NANOHUB_FIRMWARE_UPLOAD_APP_SEC_SIG_ROOT_UNKNOWN;
            break;
        case APP_SEC_MEMORY_ERROR:
            resp->uploadReply = NANOHUB_FIRMWARE_UPLOAD_APP_SEC_MEMORY_ERROR;
            break;
        case APP_SEC_INVALID_DATA:
            resp->uploadReply = NANOHUB_FIRMWARE_UPLOAD_APP_SEC_INVALID_DATA;
            break;
        default:
            resp->uploadReply = NANOHUB_FIRMWARE_UPLOAD_APP_SEC_BAD;
            break;
        }
    } else if (mDownloadState->srcOffset == mDownloadState->size) {
        resp->uploadReply = NANOHUB_FIRMWARE_UPLOAD_PROCESSING;
    } else {
        resp->uploadReply = NANOHUB_FIRMWARE_UPLOAD_WAITING_FOR_DATA;
    }

    return sizeof(*resp);
}

static uint32_t getInterrupt(void *rx, uint8_t rx_len, void *tx, uint64_t timestamp)
{
    struct NanohubGetInterruptRequest *req = rx;
    struct NanohubGetInterruptResponse *resp = tx;
    int i;

    if (rx_len == sizeof(struct NanohubGetInterruptRequest)) {
        for (i = 0; i < HOSTINTF_MAX_INTERRUPTS; i++) {
            if (req->clear[i/32] & (1UL << (i & 31)))
                hostIntfClearInterrupt(i);
        }
    }

    hostIntfCopyInterrupts(resp->interrupts, HOSTINTF_MAX_INTERRUPTS);

    return sizeof(*resp);
}

static uint32_t maskInterrupt(void *rx, uint8_t rx_len, void *tx, uint64_t timestamp)
{
    struct NanohubMaskInterruptRequest *req = rx;
    struct NanohubMaskInterruptResponse *resp = tx;

    hostIntfSetInterruptMask(req->interrupt);

    resp->accepted = true;
    return sizeof(*resp);
}

static uint32_t unmaskInterrupt(void *rx, uint8_t rx_len, void *tx, uint64_t timestamp)
{
    struct NanohubUnmaskInterruptRequest *req = rx;
    struct NanohubUnmaskInterruptResponse *resp = tx;

    hostIntfClearInterruptMask(req->interrupt);

    resp->accepted = true;
    return sizeof(*resp);
}

struct EvtPacket
{
    uint8_t sensType;
    uint8_t length;
    uint8_t appToHost;
    uint8_t pad;
    uint64_t timestamp;
    uint8_t data[NANOHUB_SENSOR_DATA_MAX];
} __attribute__((packed));

#define SYNC_DATAPOINTS 16
#define SYNC_RESET      10000000000ULL /* 10 seconds, ~100us drift */

struct TimeSync
{
    uint64_t lastTime;
    uint64_t delta[SYNC_DATAPOINTS];
    uint64_t avgDelta;
    uint8_t cnt;
    uint8_t tail;
} __attribute__((packed));

static uint64_t getAvgDelta(struct TimeSync *sync);

static void addDelta(struct TimeSync *sync, uint64_t apTime, uint64_t hubTime)
{
    if (apTime - sync->lastTime > SYNC_RESET) {
        sync->tail = 0;
        sync->cnt = 0;
    }

    sync->delta[sync->tail++] = apTime - hubTime;

    sync->lastTime = apTime;

    if (sync->tail >= SYNC_DATAPOINTS)
        sync->tail = 0;

    if (sync->cnt < SYNC_DATAPOINTS)
        sync->cnt ++;

    sync->avgDelta = 0ULL;
}

static uint64_t getAvgDelta(struct TimeSync *sync)
{
    int i;
    int32_t avg;

    if (!sync->cnt)
        return 0ULL;
    else if (!sync->avgDelta) {
        for (i=1, avg=0; i<sync->cnt; i++)
            avg += (int32_t)(sync->delta[i] - sync->delta[0]);
        sync->avgDelta = (avg / sync->cnt) + sync->delta[0];
    }
    return sync->avgDelta;
}

static uint32_t readEvent(void *rx, uint8_t rx_len, void *tx, uint64_t timestamp)
{
    struct NanohubReadEventRequest *req = rx;
    struct NanohubReadEventResponse *resp = tx;
    struct EvtPacket *packet = tx;
    int length, sensor, appToHost;
    uint32_t wakeup, nonwakeup;
    static struct TimeSync timeSync = { };

    addDelta(&timeSync, req->apBootTime, timestamp);

    if (hostIntfPacketDequeue(packet, &wakeup, &nonwakeup)) {
        length = packet->length + sizeof(resp->evtType);
        sensor = packet->sensType;
        appToHost = packet->appToHost;
        // TODO combine messages if multiple can fit in a single packet
        if (sensor == SENS_TYPE_INVALID) {
            if (appToHost)
                resp->evtType = EVT_APP_TO_HOST;
            else
#ifdef DEBUG_LOG_EVT
                resp->evtType = htole32(DEBUG_LOG_EVT);
#else
                resp->evtType = 0x00000000;
#endif
        } else {
            resp->evtType = htole32(EVT_NO_FIRST_SENSOR_EVENT + sensor);
            if (packet->timestamp)
                packet->timestamp += getAvgDelta(&timeSync);
            if (!wakeup)
                hostIntfClearInterrupt(NANOHUB_INT_WAKEUP);
            if (!nonwakeup)
                hostIntfClearInterrupt(NANOHUB_INT_NONWAKEUP);
        }
    } else {
        hostIntfClearInterrupt(NANOHUB_INT_WAKEUP);
        hostIntfClearInterrupt(NANOHUB_INT_NONWAKEUP);
        length = 0;
    }

    return length;
}

static uint32_t writeEvent(void *rx, uint8_t rx_len, void *tx, uint64_t timestamp)
{
    struct NanohubWriteEventRequest *req = rx;
    struct NanohubWriteEventResponse *resp = tx;
    uint8_t *packet;
    struct HostHubRawPacket *rawPacket;
    uint32_t tid;

    if (le32toh(req->evtType) == EVT_APP_FROM_HOST) {
        rawPacket = (struct HostHubRawPacket *)req->evtData;
        if (rx_len >= sizeof(req->evtType) + sizeof(struct HostHubRawPacket) && rx_len == sizeof(req->evtType) + sizeof(struct HostHubRawPacket) + rawPacket->dataLen && osTidById(rawPacket->appId, &tid)) {
            packet = heapAlloc(rawPacket->dataLen + 1);
            if (!packet) {
                resp->accepted = false;
            } else {
                packet[0] = rawPacket->dataLen;
                memcpy(packet + 1, rawPacket + 1, rawPacket->dataLen);
                resp->accepted = osEnqueuePrivateEvt(EVT_APP_FROM_HOST, packet, heapFree, tid);
                if (!resp->accepted)
                    heapFree(packet);
            }
        } else {
            resp->accepted = false;
        }
    } else {
        packet = heapAlloc(rx_len - sizeof(req->evtType));
        if (!packet) {
            resp->accepted = false;
        } else {
            memcpy(packet, req->evtData, rx_len - sizeof(req->evtType));
            resp->accepted = osEnqueueEvt(le32toh(req->evtType), packet, heapFree);
            if (!resp->accepted)
                heapFree(packet);
        }
    }

    return sizeof(*resp);
}

const static struct NanohubCommand mBuiltinCommands[] = {
    NANOHUB_COMMAND(NANOHUB_REASON_GET_OS_HW_VERSIONS,
                    getOsHwVersion,
                    struct NanohubOsHwVersionsRequest,
                    struct NanohubOsHwVersionsRequest),
    NANOHUB_COMMAND(NANOHUB_REASON_GET_APP_VERSIONS,
                    getAppVersion,
                    struct NanohubAppVersionsRequest,
                    struct NanohubAppVersionsRequest),
    NANOHUB_COMMAND(NANOHUB_REASON_QUERY_APP_INFO,
                    queryAppInfo,
                    struct NanohubAppInfoRequest,
                    struct NanohubAppInfoRequest),
    NANOHUB_COMMAND(NANOHUB_REASON_START_FIRMWARE_UPLOAD,
                    startFirmwareUpload,
                    struct NanohubStartFirmwareUploadRequest,
                    struct NanohubStartFirmwareUploadRequest),
    NANOHUB_COMMAND(NANOHUB_REASON_FIRMWARE_CHUNK,
                    firmwareChunk,
                    __le32,
                    struct NanohubFirmwareChunkRequest),
    NANOHUB_COMMAND(NANOHUB_REASON_FINISH_FIRMWARE_UPLOAD,
                    finishFirmwareUpload,
                    struct NanohubFinishFirmwareUploadRequest,
                    struct NanohubFinishFirmwareUploadRequest),
    NANOHUB_COMMAND(NANOHUB_REASON_GET_INTERRUPT,
                    getInterrupt,
                    0,
                    struct NanohubGetInterruptRequest),
    NANOHUB_COMMAND(NANOHUB_REASON_MASK_INTERRUPT,
                    maskInterrupt,
                    struct NanohubMaskInterruptRequest,
                    struct NanohubMaskInterruptRequest),
    NANOHUB_COMMAND(NANOHUB_REASON_UNMASK_INTERRUPT,
                    unmaskInterrupt,
                    struct NanohubUnmaskInterruptRequest,
                    struct NanohubUnmaskInterruptRequest),
    NANOHUB_COMMAND(NANOHUB_REASON_READ_EVENT,
                    readEvent,
                    struct NanohubReadEventRequest,
                    struct NanohubReadEventRequest),
    NANOHUB_COMMAND(NANOHUB_REASON_WRITE_EVENT,
                    writeEvent,
                    __le32,
                    struct NanohubWriteEventRequest),
};

const struct NanohubCommand *nanohubFindCommand(uint32_t packetReason)
{
    uint32_t i;

    for (i = 0; i < ARRAY_SIZE(mBuiltinCommands); i++) {
        const struct NanohubCommand *cmd = &mBuiltinCommands[i];
        if (cmd->reason == packetReason)
            return cmd;
    }
    return NULL;
}

static void halExtAppsOn(void *rx, uint8_t rx_len)
{
}

static void halExtAppsOff(void *rx, uint8_t rx_len)
{
}

static void halExtAppDelete(void *rx, uint8_t rx_len)
{
}

static void halQueryMemInfo(void *rx, uint8_t rx_len)
{
}

static void halQueryApps(void *rx, uint8_t rx_len)
{
    struct NanohubHalQueryAppsRx *req = rx;
    struct NanohubHalQueryAppsTx *resp;
    struct NanohubHalHdr *hdr;
    uint64_t appId;
    uint32_t appVer, appSize;

    if (osAppInfoByIndex(le32toh(req->idx), &appId, &appVer, &appSize)) {
        resp = heapAlloc(sizeof(*resp));
        resp->hdr.appId = APP_ID_MAKE(APP_ID_VENDOR_GOOGLE, 0);
        resp->hdr.len = sizeof(*resp) - sizeof(struct NanohubHalHdr) + 1;
        resp->hdr.msg = NANOHUB_HAL_QUERY_APPS;
        resp->appId = appId;
        resp->version = appVer;
        resp->flashUse = appSize;
        resp->ramUse = 0;
        osEnqueueEvt(EVT_APP_TO_HOST, resp, heapFree);
    } else {
        hdr = heapAlloc(sizeof(*hdr));
        hdr->appId = APP_ID_MAKE(APP_ID_VENDOR_GOOGLE, 0);
        hdr->len = 1;
        hdr->msg = NANOHUB_HAL_QUERY_APPS;
        osEnqueueEvt(EVT_APP_TO_HOST, hdr, heapFree);
    }
}

static void halQueryRsaKeys(void *rx, uint8_t rx_len)
{
    struct NanohubHalQueryRsaKeysRx *req = rx;
    struct NanohubHalQueryRsaKeysTx *resp;
    int len = 0;
    const uint32_t *ptr;
    uint32_t numKeys;

    if (!(resp = heapAlloc(sizeof(*resp) + NANOHUB_RSA_KEY_CHUNK_LEN)))
        return;

    ptr = BL.blGetPubKeysInfo(&numKeys);
    if (ptr && numKeys * RSA_BYTES > req->offset) {
        len = numKeys * RSA_BYTES - req->offset;
        if (len > NANOHUB_RSA_KEY_CHUNK_LEN)
            len = NANOHUB_RSA_KEY_CHUNK_LEN;
        memcpy(resp->data, (uint8_t *)ptr + req->offset, len);
    }

    resp->hdr.appId = APP_ID_MAKE(APP_ID_VENDOR_GOOGLE, 0);
    resp->hdr.len = sizeof(*resp) - sizeof(struct NanohubHalHdr) + 1 + len;
    resp->hdr.msg = NANOHUB_HAL_QUERY_RSA_KEYS;

    if (!(osEnqueueEvt(EVT_APP_TO_HOST, resp, heapFree)))
        heapFree(resp);
}

static void halStartUpload(void *rx, uint8_t rx_len)
{
    struct NanohubHalStartUploadRx *req = rx;
    struct NanohubHalStartUploadTx *resp;
    uint8_t *shared, *shared_start, *shared_end;
    int len, total_len;
    uint32_t sharedSz;

    if (!(resp = heapAlloc(sizeof(*resp))))
        return;

    resp->hdr.appId = APP_ID_MAKE(APP_ID_VENDOR_GOOGLE, 0);
    resp->hdr.len = sizeof(*resp) - sizeof(struct NanohubHalHdr) + 1;
    resp->hdr.msg = NANOHUB_HAL_START_UPLOAD;

    shared_start = platGetSharedAreaInfo(&sharedSz);
    shared_end = shared_start + sharedSz;

    if (!mDownloadState) {
        mDownloadState = heapAlloc(sizeof(struct DownloadState));

        if (!mDownloadState) {
            resp->success = false;
            if (!(osEnqueueEvt(EVT_APP_TO_HOST, resp, heapFree)))
                heapFree(resp);
            return;
        } else {
            memset(mDownloadState, 0x00, sizeof(struct DownloadState));
        }
    }

    mDownloadState->type = req->isOs ? BL_FLASH_KERNEL_ID : BL_FLASH_APP_ID;
    mDownloadState->size = le32toh(req->length);
    mDownloadState->crc = le32toh(0x00000000);
    mDownloadState->chunkReply = NANOHUB_FIRMWARE_CHUNK_REPLY_ACCEPTED;

    for (shared = shared_start;
         shared < shared_end && shared[0] != 0xFF;
         shared += total_len) {
        len = (shared[1] << 16) | (shared[2] << 8) | shared[3];
        total_len = sizeof(uint32_t) + ((len + 3) & ~3) + sizeof(uint32_t);
    }

    if (shared + sizeof(uint32_t) + ((mDownloadState->size + 3) & ~3) + sizeof(uint32_t) < shared_end) {
        mDownloadState->start = shared;
        mDownloadState->erase = false;
    } else {
        mDownloadState->start = shared_start;
        mDownloadState->erase = true;
    }
    resetDownloadState();

    resp->success = true;
    if (!(osEnqueueEvt(EVT_APP_TO_HOST, resp, heapFree)))
        heapFree(resp);
}

static void halContUpload(void *rx, uint8_t rx_len)
{
    struct NanohubHalContUploadRx *req = rx;
    struct NanohubHalContUploadTx *resp;
    uint32_t offset;
    uint8_t len;

    if (!(resp = heapAlloc(sizeof(*resp))))
        return;

    resp->hdr.appId = APP_ID_MAKE(APP_ID_VENDOR_GOOGLE, 0);
    resp->hdr.len = sizeof(*resp) - sizeof(struct NanohubHalHdr) + 1;
    resp->hdr.msg = NANOHUB_HAL_CONT_UPLOAD;

    offset = le32toh(req->offset);
    len = rx_len - sizeof(req->offset);

    if (!mDownloadState) {
        resp->success = NANOHUB_FIRMWARE_CHUNK_REPLY_CANCEL_NO_RETRY;
    } else if (mDownloadState->erase == true) {
        resp->success = NANOHUB_FIRMWARE_CHUNK_REPLY_WAIT;
        firmwareErase(NULL);
    } else if (offset != mDownloadState->srcOffset) {
        resp->success = NANOHUB_FIRMWARE_CHUNK_REPLY_RESTART;
        resetDownloadState();
    } else if (mDownloadState->srcOffset + len <= mDownloadState->size) {
        mDownloadState->srcOffset += len;
        memcpy(mDownloadState->data, req->data, len);
        mDownloadState->len = len;
        hostIntfSetBusy(true);
        firmwareWrite(NULL);
        if (mDownloadState)
            resp->success = mDownloadState->chunkReply;
        else
            resp->success = NANOHUB_FIRMWARE_CHUNK_REPLY_ACCEPTED;
    } else {
        resp->success = NANOHUB_FIRMWARE_CHUNK_REPLY_CANCEL_NO_RETRY;
    }
    resp->success = !resp->success;

    if (!(osEnqueueEvt(EVT_APP_TO_HOST, resp, heapFree)))
        heapFree(resp);
}

static void halFinishUpload(void *rx, uint8_t rx_len)
{
    struct NanohubHalFinishUploadTx *resp;

    if (!(resp = heapAlloc(sizeof(*resp))))
        return;

    resp->hdr.appId = APP_ID_MAKE(APP_ID_VENDOR_GOOGLE, 0);
    resp->hdr.len = sizeof(*resp) - sizeof(struct NanohubHalHdr) + 1;
    resp->hdr.msg = NANOHUB_HAL_FINISH_UPLOAD;

    if (!mDownloadState) {
        switch (mAppSecStatus) {
        case APP_SEC_NO_ERROR:
            resp->success = NANOHUB_FIRMWARE_UPLOAD_SUCCESS;
            break;
        case APP_SEC_KEY_NOT_FOUND:
            resp->success = NANOHUB_FIRMWARE_UPLOAD_APP_SEC_KEY_NOT_FOUND;
            break;
        case APP_SEC_HEADER_ERROR:
            resp->success = NANOHUB_FIRMWARE_UPLOAD_APP_SEC_HEADER_ERROR;
            break;
        case APP_SEC_TOO_MUCH_DATA:
            resp->success = NANOHUB_FIRMWARE_UPLOAD_APP_SEC_TOO_MUCH_DATA;
            break;
        case APP_SEC_TOO_LITTLE_DATA:
            resp->success = NANOHUB_FIRMWARE_UPLOAD_APP_SEC_TOO_LITTLE_DATA;
            break;
        case APP_SEC_SIG_VERIFY_FAIL:
            resp->success = NANOHUB_FIRMWARE_UPLOAD_APP_SEC_SIG_VERIFY_FAIL;
            break;
        case APP_SEC_SIG_DECODE_FAIL:
            resp->success = NANOHUB_FIRMWARE_UPLOAD_APP_SEC_SIG_DECODE_FAIL;
            break;
        case APP_SEC_SIG_ROOT_UNKNOWN:
            resp->success = NANOHUB_FIRMWARE_UPLOAD_APP_SEC_SIG_ROOT_UNKNOWN;
            break;
        case APP_SEC_MEMORY_ERROR:
            resp->success = NANOHUB_FIRMWARE_UPLOAD_APP_SEC_MEMORY_ERROR;
            break;
        case APP_SEC_INVALID_DATA:
            resp->success = NANOHUB_FIRMWARE_UPLOAD_APP_SEC_INVALID_DATA;
            break;
        default:
            resp->success = NANOHUB_FIRMWARE_UPLOAD_APP_SEC_BAD;
            break;
        }
    } else if (mDownloadState->srcOffset == mDownloadState->size) {
        resp->success = NANOHUB_FIRMWARE_UPLOAD_PROCESSING;
    } else {
        resp->success = NANOHUB_FIRMWARE_UPLOAD_WAITING_FOR_DATA;
    }
    resp->success = !resp->success;

    if (!(osEnqueueEvt(EVT_APP_TO_HOST, resp, heapFree)))
        heapFree(resp);
}

static void halReboot(void *rx, uint8_t rx_len)
{
    BL.blReboot();
}

const static struct NanohubHalCommand mBuiltinHalCommands[] = {
    NANOHUB_HAL_COMMAND(NANOHUB_HAL_EXT_APPS_ON,
                        halExtAppsOn),
    NANOHUB_HAL_COMMAND(NANOHUB_HAL_EXT_APPS_OFF,
                        halExtAppsOff),
    NANOHUB_HAL_COMMAND(NANOHUB_HAL_EXT_APP_DELETE,
                        halExtAppDelete),
    NANOHUB_HAL_COMMAND(NANOHUB_HAL_QUERY_MEMINFO,
                        halQueryMemInfo),
    NANOHUB_HAL_COMMAND(NANOHUB_HAL_QUERY_APPS,
                        halQueryApps),
    NANOHUB_HAL_COMMAND(NANOHUB_HAL_QUERY_RSA_KEYS,
                        halQueryRsaKeys),
    NANOHUB_HAL_COMMAND(NANOHUB_HAL_START_UPLOAD,
                        halStartUpload),
    NANOHUB_HAL_COMMAND(NANOHUB_HAL_CONT_UPLOAD,
                        halContUpload),
    NANOHUB_HAL_COMMAND(NANOHUB_HAL_FINISH_UPLOAD,
                        halFinishUpload),
    NANOHUB_HAL_COMMAND(NANOHUB_HAL_REBOOT,
                        halReboot),
};

const struct NanohubHalCommand *nanohubHalFindCommand(uint8_t msg)
{
    uint32_t i;

    for (i = 0; i < ARRAY_SIZE(mBuiltinHalCommands); i++) {
        const struct NanohubHalCommand *cmd = &mBuiltinHalCommands[i];
        if (cmd->msg == msg)
            return cmd;
    }
    return NULL;
}
