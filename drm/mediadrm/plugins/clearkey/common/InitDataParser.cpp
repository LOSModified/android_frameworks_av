/*
 * Copyright (C) 2021 The Android Open Source Project
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
//#define LOG_NDEBUG 0
#define LOG_TAG "clearkey-InitDataParser"

#include <algorithm>
#include <arpa/inet.h>
#include <utils/Log.h>

#include "InitDataParser.h"

#include "Base64.h"

#include "ClearKeyUUID.h"
#include "MimeType.h"

namespace {
const size_t kKeyIdSize = 16;
const size_t kSystemIdSize = 16;
}  // namespace

namespace clearkeydrm {

std::vector<uint8_t> StrToVector(const std::string& str) {
    std::vector<uint8_t> vec(str.begin(), str.end());
    return vec;
}

CdmResponseType InitDataParser::parse(const std::vector<uint8_t>& initData,
                                      const std::string& mimeType,
                                      CdmKeyType keyType,
                                      std::vector<uint8_t>* licenseRequest) {
    // Build a list of the key IDs
    std::vector<const uint8_t*> keyIds;

    if (mimeType == kIsoBmffVideoMimeType.c_str() || mimeType == kIsoBmffAudioMimeType.c_str() ||
        mimeType == kCencInitDataFormat.c_str()) {
        auto res = parsePssh(initData, &keyIds);
        if (res != clearkeydrm::OK) {
            return res;
        }
    } else if (mimeType == kWebmVideoMimeType.c_str() || mimeType == kWebmAudioMimeType.c_str() ||
               mimeType == kWebmInitDataFormat.c_str()) {
        // WebM "init data" is just a single key ID
        if (initData.size() != kKeyIdSize) {
            return clearkeydrm::ERROR_CANNOT_HANDLE;
        }
        keyIds.push_back(initData.data());
    } else {
        return clearkeydrm::ERROR_CANNOT_HANDLE;
    }

    if (keyType == clearkeydrm::KEY_TYPE_RELEASE) {
        // restore key
    }

    // Build the request
    std::string requestJson = generateRequest(keyType, keyIds);
    std::vector<uint8_t> requestJsonVec = StrToVector(requestJson);

    licenseRequest->clear();
    licenseRequest->insert(licenseRequest->end(), requestJsonVec.begin(), requestJsonVec.end());
    return clearkeydrm::OK;
}

CdmResponseType InitDataParser::parsePssh(const std::vector<uint8_t>& initData,
                                          std::vector<const uint8_t*>* keyIds) {
    // Description of PSSH format:
    // https://w3c.github.io/encrypted-media/format-registry/initdata/cenc.html
    size_t readPosition = 0;

    uint32_t expectedSize = initData.size();
    const char psshIdentifier[4] = {'p', 's', 's', 'h'};
    const uint8_t psshVersion1[4] = {1, 0, 0, 0};
    uint32_t keyIdCount = 0;
    size_t headerSize = sizeof(expectedSize) + sizeof(psshIdentifier) + sizeof(psshVersion1) +
                        kSystemIdSize + sizeof(keyIdCount);
    if (initData.size() < headerSize) {
        return clearkeydrm::ERROR_CANNOT_HANDLE;
    }

    // Validate size field
    expectedSize = htonl(expectedSize);
    if (memcmp(&initData[readPosition], &expectedSize, sizeof(expectedSize)) != 0) {
        return clearkeydrm::ERROR_CANNOT_HANDLE;
    }
    readPosition += sizeof(expectedSize);

    // Validate PSSH box identifier
    if (memcmp(&initData[readPosition], psshIdentifier, sizeof(psshIdentifier)) != 0) {
        return clearkeydrm::ERROR_CANNOT_HANDLE;
    }
    readPosition += sizeof(psshIdentifier);

    // Validate EME version number
    if (memcmp(&initData[readPosition], psshVersion1, sizeof(psshVersion1)) != 0) {
        return clearkeydrm::ERROR_CANNOT_HANDLE;
    }
    readPosition += sizeof(psshVersion1);

    // Validate system ID
    if (!isClearKeyUUID(&initData[readPosition])) {
        return clearkeydrm::ERROR_CANNOT_HANDLE;
    }
    readPosition += kSystemIdSize;

    // Read key ID count
    memcpy(&keyIdCount, &initData[readPosition], sizeof(keyIdCount));
    keyIdCount = ntohl(keyIdCount);
    readPosition += sizeof(keyIdCount);

    uint64_t psshSize = 0;
    if (__builtin_mul_overflow(keyIdCount, kKeyIdSize, &psshSize) ||
        __builtin_add_overflow(readPosition, psshSize, &psshSize) ||
        psshSize != initData.size() - sizeof(uint32_t) /* DataSize(0) */) {
        return clearkeydrm::ERROR_CANNOT_HANDLE;
    }

    // Calculate the key ID offsets
    for (uint32_t i = 0; i < keyIdCount; ++i) {
        size_t keyIdPosition = readPosition + (i * kKeyIdSize);
        keyIds->push_back(&initData[keyIdPosition]);
    }
    return clearkeydrm::OK;
}

std::string InitDataParser::generateRequest(CdmKeyType keyType,
                                            const std::vector<const uint8_t*>& keyIds) {
    const std::string kRequestPrefix("{\"kids\":[");
    const std::string kTemporarySession("],\"type\":\"temporary\"}");
    const std::string kPersistentSession("],\"type\":\"persistent-license\"}");

    std::string request(kRequestPrefix);
    std::string encodedId;
    for (size_t i = 0; i < keyIds.size(); ++i) {
        encodedId.clear();
        encodeBase64Url(keyIds[i], kKeyIdSize, &encodedId);
        if (i != 0) {
            request.append(",");
        }
        request.push_back('\"');
        request.append(encodedId);
        request.push_back('\"');
    }
    if (keyType == clearkeydrm::KEY_TYPE_STREAMING) {
        request.append(kTemporarySession);
    } else if (keyType == clearkeydrm::KEY_TYPE_OFFLINE ||
               keyType == clearkeydrm::KEY_TYPE_RELEASE) {
        request.append(kPersistentSession);
    }

    // Android's Base64 encoder produces padding. EME forbids padding.
    const char kBase64Padding = '=';
    request.erase(std::remove(request.begin(), request.end(), kBase64Padding), request.end());

    return request;
}

}  // namespace clearkeydrm
