#pragma once
// IConnectFs portável, só para o cbmp-headless: o connect.json é um arquivo comum (sem trava exclusiva do Win32).
#include <cbmp/client/ConnectFile.hpp>
#include <cbmp/client/Limits.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace cbmp::headless {
class FileConnectFs final : public cbmp::client::IConnectFs {
public:
    explicit FileConnectFs(std::filesystem::path path) : m_path(std::move(path)) {}

    bool Exists() override {
        std::error_code ec;
        return std::filesystem::is_regular_file(m_path, ec);
    }

    // Lê (≤ 16 KiB) e apaga. Maior que isso: recusa e apaga do mesmo jeito (o ticket não fica no disco).
    cbmp::client::ClaimResult ClaimAndDelete() override {
        using cbmp::client::ClaimRead;
        cbmp::client::ClaimResult r;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(m_path, ec)) return r;  // Missing
        const auto size = std::filesystem::file_size(m_path, ec);
        if (ec) {
            r.status = ClaimRead::Busy;
            return r;
        }
        if (size > cbmp::client::limits::kMaxConnectBytes) {
            std::filesystem::remove(m_path, ec);
            r.status = ClaimRead::Rejected;
            return r;
        }
        {
            std::ifstream in(m_path, std::ios::binary);
            if (!in) {
                r.status = ClaimRead::Busy;
                return r;
            }
            r.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        std::filesystem::remove(m_path, ec);
        r.status = ClaimRead::Ok;
        return r;
    }

private:
    std::filesystem::path m_path;
};
}  // namespace cbmp::headless
