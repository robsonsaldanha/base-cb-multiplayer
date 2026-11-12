#include <catch2/catch_test_macros.hpp>
#include <cbmp/client/Limits.hpp>

using namespace cbmp::client::limits;

// Valores da spec (Global Constraints do sub5a): um REQUIRE por constante.
static_assert(kMaxMessageBytes == 8192);
static_assert(kEndRejected >= 1000);  // k_ESteamNetConnectionEnd_App_Min = 1000

TEST_CASE("client: limites de mensagens e entidades") {
    REQUIRE(kMaxMessageBytes == 8192);
    REQUIRE(kMaxMessagesPerTick == 512);
    REQUIRE(kMaxPendingMessages == 4096);
    REQUIRE(kMaxEntities == 255);
    REQUIRE(kHistoryTicks == 32);
    REQUIRE(kMaxParts == 8);
    REQUIRE(kMaxNameBytes == 64);
    REQUIRE(kMaxReasonBytes == 255);
    REQUIRE(kMaxPointerBytes == 4096);
    REQUIRE(kMaxConnectBytes == 16384);
    REQUIRE(kInterpQueue == 32);
    REQUIRE(kLogKeep == 10);
}

TEST_CASE("client: limites inteiros") {
    REQUIRE(kMaxDecodeErrors == 20);
    REQUIRE(kPointerSearchLevels == 5);
    REQUIRE(kMinTickRateHz == 5);
    REQUIRE(kMaxTickRateHz == 60);
    REQUIRE(kDefaultSendHz == 20);
    REQUIRE(kDefaultMaxPuppets == 32);
    REQUIRE(kHardMaxPuppets == 64);
    REQUIRE(kSpawnRetries == 3);
    REQUIRE(kMaxRecreatesPerMinute == 5);
    REQUIRE(kMoveErrorsToGone == 3);
    REQUIRE(kGameErrorsToDisable == 20);
    REQUIRE(kTickExceptionLimit == 100);
    REQUIRE(kMaxVelCmS == 32767);
}

TEST_CASE("client: limites de tempo do ponteiro e do ticket") {
    REQUIRE(kPointerMaxAgeSec == 600);
    REQUIRE(kPointerMaxFutureSec == 120);
    REQUIRE(kTicketMinRemainingSec == 10);
}

TEST_CASE("client: prazos da sessao") {
    REQUIRE(kDecodeErrorWindowSec == 10);
    REQUIRE(kClaimDelayFirstSec == 45);
    REQUIRE(kClaimDelaySuccessorSec == 5);
    REQUIRE(kClaimRetrySec == 5);
    REQUIRE(kConnectTimeoutSec == 10);
    REQUIRE(kHelloDeadlineSec == 1);
    REQUIRE(kWelcomeTimeoutSec == 10);
}

TEST_CASE("client: limites de interpolacao e relogio") {
    REQUIRE(kInterpDelaySec == 0.100);
    REQUIRE(kMaxInterpDelaySec == 0.400);
    REQUIRE(kMaxExtrapolationSec == 0.250);
    REQUIRE(kSnapDistanceCm == 500);
    REQUIRE(kClockWindowSec == 10);
    REQUIRE(kClockResetSec == 2);
}

TEST_CASE("client: limites de correcao e estado local") {
    REQUIRE(kCorrectionIgnoreCm == 100);
    REQUIRE(kCorrectionTeleportMaxCm == 2000);
    REQUIRE(kTeleportMinIntervalSec == 1);
    REQUIRE(kMaxCoordCm == 2'000'000'000.0);
    REQUIRE(kJumpDistanceCm == 3000);
    REQUIRE(kIdleMaxCmS == 10);
    REQUIRE(kWalkMaxCmS == 380);
    REQUIRE(kRunMaxCmS == 1000);
    REQUIRE(kInWorldStableSec == 2);
    REQUIRE(kOriginRadiusCm == 100);
}

TEST_CASE("client: limites dos bonecos e dos contadores") {
    REQUIRE(kPuppetRadiusCm == 40000);
    REQUIRE(kSpawnRetrySec == 1);
    REQUIRE(kRecreateDelaySec == 1);
    REQUIRE(kGameErrorWindowSec == 10);
    REQUIRE(kTickExceptionWindowSec == 60);
    REQUIRE(kExceptionSummarySec == 10);
    REQUIRE(kStatsIntervalSec == 10);
}

TEST_CASE("client: codigos de fim de conexao") {
    REQUIRE(kEndRejected == 1001);
    REQUIRE(kEndLimits == 1002);
    REQUIRE(kEndProtocol == 1003);
    REQUIRE(kEndKicked == 1004);
}
