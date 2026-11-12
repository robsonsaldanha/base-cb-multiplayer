#include "FileHash.hpp"
#include <Windows.h>
#include <bcrypt.h>
#include <format>
#include <fstream>
#include <vector>

namespace cbmp::client {
bool Sha256Bytes(const std::filesystem::path& file, std::array<uint8_t, 32>& out) {
    out.fill(0);
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    BCRYPT_ALG_HANDLE alg{};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
    bool done = false;
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buf(1 << 20);
        bool ok = true;
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const auto n = static_cast<ULONG>(in.gcount());
            if (n && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), n, 0) != 0) {
                ok = false;
                break;
            }
        }
        if (in.bad()) ok = false;
        std::array<uint8_t, 32> digest{};
        if (ok && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0) {
            out = digest;
            done = true;
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return done;
}

std::string Sha256Hex(const std::filesystem::path& file) {
    std::array<uint8_t, 32> digest{};
    if (!Sha256Bytes(file, digest)) return "erro";
    std::string result;
    for (uint8_t b : digest) result += std::format("{:02x}", b);
    return result;
}
}  // namespace cbmp::client
