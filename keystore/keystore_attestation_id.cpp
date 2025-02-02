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

#include <keystore/keystore_attestation_id.h>

#define LOG_TAG "keystore_att_id"

#include <log/log.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
#include <binder/Parcelable.h>
#include <binder/PersistableBundle.h>

#include <aidl/android/system/keystore2/ResponseCode.h>
#include <android/security/keystore/BpKeyAttestationApplicationIdProvider.h>
#include <android/security/keystore/IKeyAttestationApplicationIdProvider.h>
#include <android/security/keystore/KeyAttestationApplicationId.h>
#include <android/security/keystore/KeyAttestationPackageInfo.h>
#include <android/security/keystore/Signature.h>

#include <private/android_filesystem_config.h> /* for AID_SYSTEM */

#include <openssl/asn1t.h>
#include <openssl/bn.h>
#include <openssl/sha.h>

#include <utils/String8.h>

namespace android {

namespace {

constexpr const char* kAttestationSystemPackageName = "AndroidSystem";
constexpr const size_t kMaxAttempts = 3;
constexpr const unsigned long kRetryIntervalUsecs = 500000;  // sleep for 500 ms
constexpr const char* kProviderServiceName = "sec_key_att_app_id_provider";

std::vector<uint8_t> signature2SHA256(const security::keystore::Signature& sig) {
    std::vector<uint8_t> digest_buffer(SHA256_DIGEST_LENGTH);
    SHA256(sig.data.data(), sig.data.size(), digest_buffer.data());
    return digest_buffer;
}

using ::aidl::android::system::keystore2::ResponseCode;
using ::android::security::keystore::BpKeyAttestationApplicationIdProvider;

[[clang::no_destroy]] std::mutex gServiceMu;
[[clang::no_destroy]] std::shared_ptr<BpKeyAttestationApplicationIdProvider>
    gService;  // GUARDED_BY gServiceMu

std::shared_ptr<BpKeyAttestationApplicationIdProvider> get_service() {
    std::lock_guard<std::mutex> guard(gServiceMu);
    if (gService.get() == nullptr) {
        gService = std::make_shared<BpKeyAttestationApplicationIdProvider>(
            android::defaultServiceManager()->waitForService(String16(kProviderServiceName)));
    }
    return gService;
}

void reset_service() {
    std::lock_guard<std::mutex> guard(gServiceMu);
    // Drop the global reference; any thread that already has a reference can keep using it.
    gService.reset();
}

DECLARE_STACK_OF(ASN1_OCTET_STRING);

typedef struct km_attestation_package_info {
    ASN1_OCTET_STRING* package_name;
    ASN1_INTEGER* version;
} KM_ATTESTATION_PACKAGE_INFO;

// Estimated size:
// 4 bytes for the package name + package_name length
// 11 bytes for the version (2 bytes header and up to 9 bytes of data).
constexpr size_t AAID_PKG_INFO_OVERHEAD = 15;
ASN1_SEQUENCE(KM_ATTESTATION_PACKAGE_INFO) = {
    ASN1_SIMPLE(KM_ATTESTATION_PACKAGE_INFO, package_name, ASN1_OCTET_STRING),
    ASN1_SIMPLE(KM_ATTESTATION_PACKAGE_INFO, version, ASN1_INTEGER),
} ASN1_SEQUENCE_END(KM_ATTESTATION_PACKAGE_INFO);
IMPLEMENT_ASN1_FUNCTIONS(KM_ATTESTATION_PACKAGE_INFO);

DECLARE_STACK_OF(KM_ATTESTATION_PACKAGE_INFO);

// Estimated size:
// See estimate above for the stack of package infos.
// 34 (32 + 2) bytes for each signature digest.
constexpr size_t AAID_SIGNATURE_SIZE = 34;
typedef struct km_attestation_application_id {
    STACK_OF(KM_ATTESTATION_PACKAGE_INFO) * package_infos;
    STACK_OF(ASN1_OCTET_STRING) * signature_digests;
} KM_ATTESTATION_APPLICATION_ID;

// Estimated overhead:
// 4 for the header of the octet string containing the fully-encoded data.
// 4 for the sequence header.
// 4 for the header of the package info set.
// 4 for the header of the signature set.
constexpr size_t AAID_GENERAL_OVERHEAD = 16;
ASN1_SEQUENCE(KM_ATTESTATION_APPLICATION_ID) = {
    ASN1_SET_OF(KM_ATTESTATION_APPLICATION_ID, package_infos, KM_ATTESTATION_PACKAGE_INFO),
    ASN1_SET_OF(KM_ATTESTATION_APPLICATION_ID, signature_digests, ASN1_OCTET_STRING),
} ASN1_SEQUENCE_END(KM_ATTESTATION_APPLICATION_ID);
IMPLEMENT_ASN1_FUNCTIONS(KM_ATTESTATION_APPLICATION_ID);

}  // namespace

}  // namespace android

namespace std {
template <> struct default_delete<android::KM_ATTESTATION_PACKAGE_INFO> {
    void operator()(android::KM_ATTESTATION_PACKAGE_INFO* p) {
        android::KM_ATTESTATION_PACKAGE_INFO_free(p);
    }
};
template <> struct default_delete<ASN1_OCTET_STRING> {
    void operator()(ASN1_OCTET_STRING* p) { ASN1_OCTET_STRING_free(p); }
};
template <> struct default_delete<android::KM_ATTESTATION_APPLICATION_ID> {
    void operator()(android::KM_ATTESTATION_APPLICATION_ID* p) {
        android::KM_ATTESTATION_APPLICATION_ID_free(p);
    }
};
}  // namespace std

namespace android {
namespace security {
namespace {

using ::android::security::keystore::KeyAttestationApplicationId;
using ::android::security::keystore::KeyAttestationPackageInfo;

status_t build_attestation_package_info(const KeyAttestationPackageInfo& pinfo,
    std::unique_ptr<KM_ATTESTATION_PACKAGE_INFO>* attestation_package_info_ptr) {

    if (!attestation_package_info_ptr) return BAD_VALUE;
    auto& attestation_package_info = *attestation_package_info_ptr;

    attestation_package_info.reset(KM_ATTESTATION_PACKAGE_INFO_new());
    if (!attestation_package_info.get()) return NO_MEMORY;

    if (!pinfo.packageName) {
        ALOGE("Key attestation package info lacks package name");
        return BAD_VALUE;
    }

    std::string pkg_name(String8(pinfo.packageName).c_str());
    if (!ASN1_OCTET_STRING_set(attestation_package_info->package_name,
                               reinterpret_cast<const unsigned char*>(pkg_name.data()),
                               pkg_name.size())) {
        return UNKNOWN_ERROR;
    }

    BIGNUM* bn_version = BN_new();
    if (bn_version == nullptr) {
        return NO_MEMORY;
    }
    if (BN_set_u64(bn_version, static_cast<uint64_t>(pinfo.versionCode)) != 1) {
        BN_free(bn_version);
        return UNKNOWN_ERROR;
    }
    status_t retval = NO_ERROR;
    if (BN_to_ASN1_INTEGER(bn_version, attestation_package_info->version) == nullptr) {
        retval = UNKNOWN_ERROR;
    }
    BN_free(bn_version);
    return retval;
}

/* The following function are not used. They are mentioned here to silence
 * warnings about them not being used.
 */
void unused_functions_silencer() __attribute__((unused));
void unused_functions_silencer() {
    i2d_KM_ATTESTATION_PACKAGE_INFO(nullptr, nullptr);
    d2i_KM_ATTESTATION_APPLICATION_ID(nullptr, nullptr, 0);
    d2i_KM_ATTESTATION_PACKAGE_INFO(nullptr, nullptr, 0);
}

}  // namespace

StatusOr<std::vector<uint8_t>>
build_attestation_application_id(const KeyAttestationApplicationId& key_attestation_id) {
    auto attestation_id =
        std::unique_ptr<KM_ATTESTATION_APPLICATION_ID>(KM_ATTESTATION_APPLICATION_ID_new());
    size_t estimated_encoded_size = AAID_GENERAL_OVERHEAD;

    auto attestation_pinfo_stack = reinterpret_cast<_STACK*>(attestation_id->package_infos);

    if (key_attestation_id.packageInfos.begin() == key_attestation_id.packageInfos.end())
        return BAD_VALUE;

    for (auto pinfo = key_attestation_id.packageInfos.begin();
         pinfo != key_attestation_id.packageInfos.end(); ++pinfo) {
        if (!pinfo->packageName) {
            ALOGE("Key attestation package info lacks package name");
            return BAD_VALUE;
        }
        std::string package_name(String8(pinfo->packageName).c_str());
        std::unique_ptr<KM_ATTESTATION_PACKAGE_INFO> attestation_package_info;
        auto rc = build_attestation_package_info(*pinfo, &attestation_package_info);
        if (rc != NO_ERROR) {
            ALOGE("Building DER attestation package info failed %d", rc);
            return rc;
        }
        estimated_encoded_size += AAID_PKG_INFO_OVERHEAD + package_name.size();
        if (estimated_encoded_size > KEY_ATTESTATION_APPLICATION_ID_MAX_SIZE) {
            break;
        }
        if (!sk_push(attestation_pinfo_stack, attestation_package_info.get())) {
            return NO_MEMORY;
        }
        // if push succeeded, the stack takes ownership
        attestation_package_info.release();
    }

    /** Apps can only share a uid iff they were signed with the same certificate(s). Because the
     *  signature field actually holds the signing certificate, rather than a signature, we can
     *  simply use the set of signature digests of the first package info.
     */
    const auto& pinfo = *key_attestation_id.packageInfos.begin();
    std::vector<std::vector<uint8_t>> signature_digests;

    for (auto sig = pinfo.signatures.begin(); sig != pinfo.signatures.end(); ++sig) {
        signature_digests.push_back(signature2SHA256(*sig));
    }

    auto signature_digest_stack = reinterpret_cast<_STACK*>(attestation_id->signature_digests);
    for (auto si : signature_digests) {
        estimated_encoded_size += AAID_SIGNATURE_SIZE;
        if (estimated_encoded_size > KEY_ATTESTATION_APPLICATION_ID_MAX_SIZE) {
            break;
        }
        auto asn1_item = std::unique_ptr<ASN1_OCTET_STRING>(ASN1_OCTET_STRING_new());
        if (!asn1_item) return NO_MEMORY;
        if (!ASN1_OCTET_STRING_set(asn1_item.get(), si.data(), si.size())) {
            return UNKNOWN_ERROR;
        }
        if (!sk_push(signature_digest_stack, asn1_item.get())) {
            return NO_MEMORY;
        }
        asn1_item.release();  // if push succeeded, the stack takes ownership
    }

    int len = i2d_KM_ATTESTATION_APPLICATION_ID(attestation_id.get(), nullptr);
    if (len < 0) return UNKNOWN_ERROR;

    std::vector<uint8_t> result(len);
    uint8_t* p = result.data();
    len = i2d_KM_ATTESTATION_APPLICATION_ID(attestation_id.get(), &p);
    if (len < 0) return UNKNOWN_ERROR;

    return result;
}

StatusOr<std::vector<uint8_t>> gather_attestation_application_id(uid_t uid) {
    KeyAttestationApplicationId key_attestation_id;

    if (uid == AID_SYSTEM || uid == AID_ROOT) {
        /* Use a fixed ID for system callers */
        auto pinfo = KeyAttestationPackageInfo();
        pinfo.packageName = String16(kAttestationSystemPackageName);
        pinfo.versionCode = 1;
        key_attestation_id.packageInfos.push_back(std::move(pinfo));
    } else {
        /* Get the attestation application ID from package manager */
        ::android::binder::Status status;

        // Retry on failure.
        for (size_t attempt{0}; attempt < kMaxAttempts; ++attempt) {
            auto pm = get_service();
            status = pm->getKeyAttestationApplicationId(uid, &key_attestation_id);
            if (status.isOk()) {
                break;
            }

            if (status.exceptionCode() == binder::Status::EX_SERVICE_SPECIFIC) {
                ALOGW("Retry: get attestation ID for %d failed with service specific error: %s %d",
                      uid, status.exceptionMessage().c_str(), status.serviceSpecificErrorCode());
            } else if (status.exceptionCode() == binder::Status::EX_TRANSACTION_FAILED) {
                // If the transaction failed, drop the package manager connection so that the next
                // attempt will try again.
                ALOGW(
                    "Retry: get attestation ID for %d transaction failed, reset connection: %s %d",
                    uid, status.exceptionMessage().c_str(), status.exceptionCode());
                reset_service();
            } else {
                ALOGW("Retry: get attestation ID for %d failed with error: %s %d", uid,
                      status.exceptionMessage().c_str(), status.exceptionCode());
            }
            usleep(kRetryIntervalUsecs);
        }

        if (!status.isOk()) {
            ALOGW("package manager request for key attestation ID failed with: %s %d",
                  status.exceptionMessage().c_str(), status.exceptionCode());

            return int32_t(ResponseCode::GET_ATTESTATION_APPLICATION_ID_FAILED);
        }
    }

    /* DER encode the attestation application ID */
    return build_attestation_application_id(key_attestation_id);
}

}  // namespace security
}  // namespace android
