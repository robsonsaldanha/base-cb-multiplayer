#pragma once
// Todos os limites e prazos do cliente num só lugar (spec do sub5a, seção "Segurança" e Global Constraints).
#include <cstddef>
#include <cstdint>

namespace cbmp::client::limits {
// Mensagens, entidades e arquivos
inline constexpr size_t kMaxMessageBytes = 8192;       // mensagem recebida
inline constexpr size_t kMaxMessagesPerTick = 512;     // processadas por tick
inline constexpr size_t kMaxPendingMessages = 4096;    // fila acima disso → desconecta
inline constexpr size_t kMaxEntities = 255;
inline constexpr size_t kHistoryTicks = 32;            // anel de snapshots
inline constexpr size_t kMaxParts = 8;                 // partCount
inline constexpr size_t kMaxNameBytes = 64;
inline constexpr size_t kMaxReasonBytes = 255;         // textos de Reject/Kick
inline constexpr size_t kMaxPointerBytes = 4096;       // cbmp-run.json
inline constexpr size_t kMaxConnectBytes = 16384;      // connect.json
inline constexpr size_t kMaxTicketBytes = 2048;        // connect.json "ticket" (= cbmp::kMaxTicketLen)
inline constexpr size_t kMaxServerIdBytes = 64;        // connect.json "serverId"
inline constexpr size_t kInterpQueue = 32;             // amostras por entidade
inline constexpr size_t kLogKeep = 10;                 // cb-mp-*.log mantidos
inline constexpr size_t kMaxModConfigBytes = 4096;     // cb-mp.json
inline constexpr size_t kMinSecretBytes = 16;          // segredo menor não é filtrado do log

// Contagens e frequências
inline constexpr int kMaxDecodeErrors = 20;            // em kDecodeErrorWindowSec → desconecta
inline constexpr int kPointerSearchLevels = 5;         // pastas acima do main.dll
inline constexpr int kMinTickRateHz = 5;
inline constexpr int kMaxTickRateHz = 60;
inline constexpr int kDefaultSendHz = 20;
inline constexpr int kDefaultMaxPuppets = 32;
inline constexpr int kHardMaxPuppets = 64;
inline constexpr int kSpawnRetries = 3;
inline constexpr int kMaxRecreatesPerMinute = 5;
inline constexpr int kMoveErrorsToGone = 3;
inline constexpr int kGameErrorsToDisable = 20;        // em kGameErrorWindowSec → bonecos desligados
inline constexpr int kTickExceptionLimit = 100;        // em kTickExceptionWindowSec → erro_interno
inline constexpr int kMaxVelCmS = 32767;

// Tempos absolutos (Unix, s)
inline constexpr int64_t kPointerMaxAgeSec = 600;
inline constexpr int64_t kPointerMaxFutureSec = 120;
inline constexpr int64_t kTicketMinRemainingSec = 10;

// Prazos (s), distâncias (cm) e velocidades (cm/s)
inline constexpr double kDecodeErrorWindowSec = 10;
inline constexpr double kClaimDelayFirstSec = 45;      // época 1
inline constexpr double kClaimDelaySuccessorSec = 5;   // época > 1
inline constexpr double kClaimRetrySec = 5;
inline constexpr double kConnectTimeoutSec = 10;
inline constexpr double kHelloDeadlineSec = 1;
inline constexpr double kWelcomeTimeoutSec = 10;
inline constexpr double kInterpDelaySec = 0.100;
inline constexpr double kMaxInterpDelaySec = 0.400;
inline constexpr double kMaxExtrapolationSec = 0.250;
inline constexpr double kSnapDistanceCm = 500;
inline constexpr double kClockWindowSec = 10;
inline constexpr double kClockResetSec = 2;
inline constexpr double kCorrectionIgnoreCm = 100;
inline constexpr double kCorrectionTeleportMaxCm = 2000;
inline constexpr double kTeleportMinIntervalSec = 1;
inline constexpr double kMaxCoordCm = 2'000'000'000.0;
inline constexpr double kJumpDistanceCm = 3000;
inline constexpr double kIdleMaxCmS = 10;
inline constexpr double kWalkMaxCmS = 380;
inline constexpr double kRunMaxCmS = 1000;
inline constexpr double kInWorldStableSec = 2;
inline constexpr double kOriginRadiusCm = 100;
inline constexpr double kPuppetRadiusCm = 40000;
inline constexpr double kSpawnRetrySec = 1;
inline constexpr double kRecreateDelaySec = 1;
inline constexpr double kGameErrorWindowSec = 10;
inline constexpr double kTickExceptionWindowSec = 60;
inline constexpr double kExceptionSummarySec = 10;
inline constexpr double kStatsIntervalSec = 10;
// Teto do log disparado por mensagens do servidor (Correction, EntityEnter, EntityLeave, Reject/Kick, Welcome fora do
// handshake, mensagens ignoradas, bonecos): até 5 linhas por categoria a cada janela de 1 s; o excedente vira uma
// linha "N linhas suprimidas (<categoria>)" por janela. Transições de estado e erros mostrados ao jogador nunca são
// suprimidos (revisão da Task 5, I3: um servidor malicioso não pode encher o disco nem travar o game thread com
// flush por linha).
inline constexpr int kServerLogLinesPerSec = 5;
// Input é não confiável: o Apparate de um salto (entrada no mundo, ressincronização) vai nos 5 primeiros Input depois
// dele (250 ms a 20 Hz), não só no primeiro. Achado pelo client-e2e com 20% de perda: o único Input com Apparate se
// perdeu e o servidor deixou o jogador na origem até a carência de 30 s.
inline constexpr int kApparateRepeatInputs = 5;
inline constexpr double kServerLogWindowSec = 1;

// Códigos de fim de conexão da aplicação (k_ESteamNetConnectionEnd_App_Min = 1000)
inline constexpr int kEndRejected = 1001;
inline constexpr int kEndLimits = 1002;
inline constexpr int kEndProtocol = 1003;
inline constexpr int kEndKicked = 1004;
}
