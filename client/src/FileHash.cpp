#include "FileHash.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include <format>
#include <fstream>
#include <vector>

std::string Sha256Hex(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return "erro";
    BCRYPT_ALG_HANDLE alg{};
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "erro";
    std::string result = "erro";
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buf(1 << 20);
        bool ok = true;
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const auto n = static_cast<ULONG>(in.gcount());
            if (n && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), n, 0) != 0) { ok = false; break; }
        }
        unsigned char digest[32]{};
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            result.clear();
            for (unsigned char b : digest) result += std::format("{:02x}", b);
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}
