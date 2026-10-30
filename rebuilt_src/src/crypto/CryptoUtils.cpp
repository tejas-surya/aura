#include "CryptoUtils.h"
#include <dpapi.h>
#include <memory>

#pragma comment(lib, "crypt32.lib")

std::vector<uint8_t> CryptoUtils::DeriveKey() {
    wchar_t computerName[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(computerName, &size);

    wchar_t userName[256]{};
    DWORD userSize = 256;
    GetUserNameW(userName, &userSize);

    std::wstring seed = std::wstring(computerName) + L":" + userName + L":MYOWNAPP_v1";
    std::vector<uint8_t> key(kAesKeySize, 0);
    for (size_t i = 0; i < seed.size(); ++i) {
        key[i % kAesKeySize] ^= static_cast<uint8_t>(seed[i]);
        key[(i + 7) % kAesKeySize] ^= static_cast<uint8_t>(seed[i] >> 8);
    }
    return key;
}

std::vector<uint8_t> CryptoUtils::EncryptAES(const std::vector<uint8_t>& plaintext) {
    if (plaintext.empty()) return {};

    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContextW(&hProv, nullptr,
        MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return {};
    }

    auto key = DeriveKey();
    auto iv = GenerateRandomBytes(kAesBlockSize);

    struct AesKeyBlob {
        BLOBHEADER header;
        DWORD keySize;
        BYTE keyData[32];
    } blob{};

    blob.header.bType = PLAINTEXTKEYBLOB;
    blob.header.bVersion = CUR_BLOB_VERSION;
    blob.header.aiKeyAlg = CALG_AES_256;
    blob.keySize = kAesKeySize;
    memcpy(blob.keyData, key.data(), kAesKeySize);

    HCRYPTKEY hKey = 0;
    if (!CryptImportKey(hProv, reinterpret_cast<BYTE*>(&blob),
        sizeof(blob), 0, 0, &hKey)) {
        CryptReleaseContext(hProv, 0);
        return {};
    }

    DWORD mode = CRYPT_MODE_CBC;
    CryptSetKeyParam(hKey, KP_MODE, reinterpret_cast<BYTE*>(&mode), 0);
    CryptSetKeyParam(hKey, KP_IV, iv.data(), 0);

    DWORD dataLen = static_cast<DWORD>(plaintext.size());
    DWORD bufLen = dataLen + kAesBlockSize;
    std::vector<uint8_t> buffer(bufLen);
    memcpy(buffer.data(), plaintext.data(), dataLen);

    if (!CryptEncrypt(hKey, 0, TRUE, 0, buffer.data(), &dataLen, bufLen)) {
        CryptDestroyKey(hKey);
        CryptReleaseContext(hProv, 0);
        return {};
    }

    buffer.resize(dataLen);

    std::vector<uint8_t> result;
    result.reserve(kAesBlockSize + dataLen);
    result.insert(result.end(), iv.begin(), iv.end());
    result.insert(result.end(), buffer.begin(), buffer.end());

    CryptDestroyKey(hKey);
    CryptReleaseContext(hProv, 0);
    return result;
}

std::vector<uint8_t> CryptoUtils::DecryptAES(const std::vector<uint8_t>& ciphertext) {
    if (ciphertext.size() <= kAesBlockSize) return {};

    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContextW(&hProv, nullptr,
        MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return {};
    }

    auto key = DeriveKey();

    std::vector<uint8_t> iv(ciphertext.begin(), ciphertext.begin() + kAesBlockSize);
    std::vector<uint8_t> data(ciphertext.begin() + kAesBlockSize, ciphertext.end());

    struct AesKeyBlob {
        BLOBHEADER header;
        DWORD keySize;
        BYTE keyData[32];
    } blob{};

    blob.header.bType = PLAINTEXTKEYBLOB;
    blob.header.bVersion = CUR_BLOB_VERSION;
    blob.header.aiKeyAlg = CALG_AES_256;
    blob.keySize = kAesKeySize;
    memcpy(blob.keyData, key.data(), kAesKeySize);

    HCRYPTKEY hKey = 0;
    if (!CryptImportKey(hProv, reinterpret_cast<BYTE*>(&blob),
        sizeof(blob), 0, 0, &hKey)) {
        CryptReleaseContext(hProv, 0);
        return {};
    }

    DWORD mode = CRYPT_MODE_CBC;
    CryptSetKeyParam(hKey, KP_MODE, reinterpret_cast<BYTE*>(&mode), 0);
    CryptSetKeyParam(hKey, KP_IV, iv.data(), 0);

    DWORD dataLen = static_cast<DWORD>(data.size());
    if (!CryptDecrypt(hKey, 0, TRUE, 0, data.data(), &dataLen)) {
        CryptDestroyKey(hKey);
        CryptReleaseContext(hProv, 0);
        return {};
    }

    data.resize(dataLen);
    CryptDestroyKey(hKey);
    CryptReleaseContext(hProv, 0);
    return data;
}

std::vector<uint8_t> CryptoUtils::EncryptDPAPI(const std::vector<uint8_t>& plaintext) {
    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(plaintext.size());
    input.pbData = const_cast<BYTE*>(plaintext.data());

    DATA_BLOB output{};
    if (!CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output)) {
        return {};
    }

    std::vector<uint8_t> result(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    return result;
}

std::vector<uint8_t> CryptoUtils::DecryptDPAPI(const std::vector<uint8_t>& ciphertext) {
    DATA_BLOB input{};
    input.cbData = static_cast<DWORD>(ciphertext.size());
    input.pbData = const_cast<BYTE*>(ciphertext.data());

    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output)) {
        return {};
    }

    std::vector<uint8_t> result(output.pbData, output.pbData + output.cbData);
    LocalFree(output.pbData);
    return result;
}

std::vector<uint8_t> CryptoUtils::GenerateRandomBytes(size_t count) {
    HCRYPTPROV hProv = 0;
    CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT);

    std::vector<uint8_t> buf(count);
    CryptGenRandom(hProv, static_cast<DWORD>(count), buf.data());

    CryptReleaseContext(hProv, 0);
    return buf;
}
