#include "huamiupdatefirmwareoperation2020.h"
#include"typeconversion.h"
#include "bipfirmwareservice.h"

constexpr uint8_t HuamiUpdateFirmwareOperation2020::COMMAND_REQUEST_PARAMETERS;
constexpr uint8_t HuamiUpdateFirmwareOperation2020::COMMAND_SEND_FIRMWARE_INFO;
constexpr uint8_t HuamiUpdateFirmwareOperation2020::COMMAND_START_TRANSFER;
constexpr uint8_t HuamiUpdateFirmwareOperation2020::REPLY_UPDATE_PROGRESS;
constexpr uint8_t HuamiUpdateFirmwareOperation2020::COMMAND_COMPLETE_TRANSFER;
constexpr uint8_t HuamiUpdateFirmwareOperation2020::COMMAND_FINALIZE_UPDATE;


HuamiUpdateFirmwareOperation2020::HuamiUpdateFirmwareOperation2020(const AbstractFirmwareInfo *info, QBLEService *service) : UpdateFirmwareOperation(info, service)
{
    qDebug() << Q_FUNC_INFO;
}

bool HuamiUpdateFirmwareOperation2020::handleMetaData(const QByteArray &value)
{
    qDebug() << Q_FUNC_INFO << value.toHex();

    if (!(value.length() == 3 || value.length() == 6 || value.length() == 11)) {
        qDebug() << "Notifications should be 3, 6 or 11 bytes long.";
        return true;
    }
    bool success = ((value[2] & 0x0f) == BipFirmwareService::SUCCESS) || ((value[1] == REPLY_UPDATE_PROGRESS) && value.length() == 6); // ugly

    if (value[0] == BipFirmwareService::RESPONSE && success) {
        switch (value[1]) {
        case COMMAND_REQUEST_PARAMETERS: {
            mChunkLength = TypeConversion::toInt16(value[4], value[5]);
            qDebug() << "got chunk length of " << mChunkLength;
            sendFwInfo();
            break;
        }
        case COMMAND_SEND_FIRMWARE_INFO: {
            sendTransferStart();
            break;
        }
        case COMMAND_START_TRANSFER: {
            sendFirmwareDataChunk(0);
            break;
        }
        case BipFirmwareService::COMMAND_FIRMWARE_START_DATA: {
            sendChecksum();
            break;
        }
        case REPLY_UPDATE_PROGRESS: {
            int offset = (value[2] & 0xff) | ((value[3] & 0xff) << 8) | ((value[4] & 0xff) << 16) | ((value[5] & 0xff) << 24);
            qDebug() << "update progress " << offset << " bytes";
            sendFirmwareDataChunk(offset);
            break;
        }
        case COMMAND_COMPLETE_TRANSFER: {
            sendFinalize();
            break;
        }
        case COMMAND_FINALIZE_UPDATE: {
            if (m_info->type() == AbstractFirmwareInfo::Firmware) {
                m_service->writeValue(BipFirmwareService::UUID_CHARACTERISTIC_FIRMWARE, QByteArray(1, BipFirmwareService::COMMAND_FIRMWARE_REBOOT));
            } else {
                m_service->message(QObject::tr("Update operation complete"));
            }
            return true;
        }
        case BipFirmwareService::COMMAND_FIRMWARE_REBOOT: {
            qDebug() << "Reboot command successfully sent.";
            m_service->message(QObject::tr("Update operation complete"));
            return true;
        }
        default: {
            qDebug() << "Unexpected response during firmware update: ";
            //operationFailed();
            m_service->message(QObject::tr("Update operation failed"));
            return true;
        }
        }
        return false;
    }

    qDebug() << "Unexpected notification during firmware update: ";
    m_service->message(QObject::tr("Update operation failed, unexpected metadata"));

    return true;
}

void HuamiUpdateFirmwareOperation2020::start()
{
    if (m_info->type() != AbstractFirmwareInfo::Invalid) {
        m_service->enableNotification(BipFirmwareService::UUID_CHARACTERISTIC_FIRMWARE);
        requestParameters();
    } else {
        m_service->message(QObject::tr("File does not seem to be supported"));
    }


}

bool HuamiUpdateFirmwareOperation2020::sendFwInfo()
{
    int fwSize = m_fwBytes.size();
    int crc32 = m_info->getCrc32();
    QByteArray sizeBytes = TypeConversion::fromInt32(fwSize);
    QByteArray crcBytes = TypeConversion::fromInt32(crc32);
    QByteArray chunkSizeBytes = TypeConversion::fromInt16(mChunkLength);

    int arraySize = 19;
    QByteArray bytes(arraySize, char(0x00));

    int i = 0;
    bytes[i++] = COMMAND_SEND_FIRMWARE_INFO;
    bytes[i++] = m_info->type();
    bytes[i++] = sizeBytes[0];
    bytes[i++] = sizeBytes[1];
    bytes[i++] = sizeBytes[2];
    bytes[i++] = sizeBytes[3];
    bytes[i++] = crcBytes[0];
    bytes[i++] = crcBytes[1];
    bytes[i++] = crcBytes[2];
    bytes[i++] = crcBytes[3];
    bytes[i++] = chunkSizeBytes[0];
    bytes[i++] = chunkSizeBytes[1];
    bytes[i++] = 0; // ??
    bytes[i++] = 0; // index
    bytes[i++] = 1; // count
    bytes[i++] = sizeBytes[0]; // total size? right now it is equal to the size above
    bytes[i++] = sizeBytes[1];
    bytes[i++] = sizeBytes[2];
    bytes[i]   = sizeBytes[3];
    qDebug() << "Sending FW info" <<bytes;

    m_service->writeValue(BipFirmwareService::UUID_CHARACTERISTIC_FIRMWARE, bytes);
    return true;
}

bool HuamiUpdateFirmwareOperation2020::requestParameters() {
    m_service->writeValue(BipFirmwareService::UUID_CHARACTERISTIC_FIRMWARE, UCHAR_TO_BYTEARRAY(COMMAND_REQUEST_PARAMETERS));
    return true;
}

bool HuamiUpdateFirmwareOperation2020::sendFirmwareDataChunk(int offset) {
    int len = m_fwBytes.size();
    int remaining = len - offset;
    int packetLength = 20; //TODO

    int chunkLength = mChunkLength;
    if (remaining < mChunkLength) {
        chunkLength = remaining;
    }

    int packets = chunkLength / packetLength;
    int chunkProgress = 0;
    int progressPercent = 0;

    BipFirmwareService *serv = dynamic_cast<BipFirmwareService*>(m_service);

    if (remaining <= 0) {
        sendTransferComplete();
        return true;
    }

    for (int i = 0; i < packets; i++) {
        QByteArray fwChunk = m_fwBytes.mid(i * packetLength, packetLength);

        m_service->writeValue(BipFirmwareService::UUID_CHARACTERISTIC_FIRMWARE_DATA, fwChunk);
        chunkProgress += packetLength;

        progressPercent = (int) ((((float) chunkProgress) / len) * 100);
        if ((i > 0) && (i % 100 == 0)) {
            serv->downloadProgress(progressPercent);
        }
        QThread::msleep(2);
    }

    if (chunkProgress < len) {
        QByteArray lastChunk = m_fwBytes.mid(packets * packetLength);
        m_service->writeValue(BipFirmwareService::UUID_CHARACTERISTIC_FIRMWARE_DATA, lastChunk);
    }

    qDebug() << "Finished sending firmware";
    serv->downloadProgress(100);

    return true;
}


void HuamiUpdateFirmwareOperation2020::sendTransferStart() {
    m_service->writeValue(BipFirmwareService::UUID_CHARACTERISTIC_FIRMWARE, UCHAR_TO_BYTEARRAY(COMMAND_START_TRANSFER) + QByteArray(1, (char)0x01));
}

void HuamiUpdateFirmwareOperation2020::sendTransferComplete() {
    m_service->writeValue(BipFirmwareService::UUID_CHARACTERISTIC_FIRMWARE, UCHAR_TO_BYTEARRAY(COMMAND_COMPLETE_TRANSFER));

}

void HuamiUpdateFirmwareOperation2020::sendFinalize() {
    m_service->writeValue(BipFirmwareService::UUID_CHARACTERISTIC_FIRMWARE, UCHAR_TO_BYTEARRAY(COMMAND_FINALIZE_UPDATE));
}
