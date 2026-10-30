#pragma once
#include <Windows.h>
#include <wincrypt.h>
#include <vector>
#include <cstdint>
#include <string>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "advapi32.lib")

class CryptoUtils {
public:
    static std::vector<uint8_t> EncryptAES(const std::vector<uint8_t>& plaintext);
    static std::vector<uint8_t> DecryptAES(const std::vector<uint8_t>& ciphertext);

    static std::vector<uint8_t> EncryptDPAPI(const std::vector<uint8_t>& plaintext);
    static std::vector<uint8_t> DecryptDPAPI(const std::vector<uint8_t>& ciphertext);

    static std::vector<uint8_t> GenerateRandomBytes(size_t count);

private:
    static constexpr DWORD kAesKeySize = 32;
    static constexpr DWORD kAesBlockSize = 16;

    static std::vector<uint8_t> DeriveKey();
};
