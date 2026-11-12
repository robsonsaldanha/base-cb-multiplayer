#pragma once
// Resgate atômico do connect.json (spec, "Dois processos na abertura"): abre sem compartilhamento, com DELETE e
// FILE_FLAG_DELETE_ON_CLOSE (o arquivo some ao fechar), confere arquivo comum, ≤ 16 KiB e dono = usuário do processo,
// lê e fecha. Win32, sem UE4SS; não inclui Windows.h no cabeçalho.
#include <cbmp/client/ConnectFile.hpp>
#include <filesystem>

namespace cbmp::client {
class WinConnectFs final : public IConnectFs {
public:
    explicit WinConnectFs(std::filesystem::path connectJson);

    bool Exists() override;                 // GetFileAttributesW: existe e não é pasta
    ClaimResult ClaimAndDelete() override;  // Missing / Busy / Rejected / Ok com os bytes

private:
    std::filesystem::path path_;
};
}  // namespace cbmp::client
