#pragma once

#include "Lib/RuntimeEpochContract.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace rts
{
namespace lockstep_v2
{

// This contract is deliberately separate from the diagnostic NET3 v1
// exchange.  A v1 file receipt can never be reinterpreted as gameplay proof.
constexpr std::uint32_t kSchemaVersion = 2U;
constexpr std::uint32_t kProtocolEpoch = 2U;
constexpr std::uint32_t kCommonStopFrame = 4096U;
constexpr std::uint32_t kMinPeerCount = 2U;
constexpr std::uint32_t kMaxPeerCount = 8U;
constexpr std::uint32_t kKernelCount = 6U;
constexpr std::uint32_t kCheckpointStride = 32U;
constexpr std::uint32_t kMaxCheckpoints =
	kCommonStopFrame / kCheckpointStride + 1U;
constexpr std::uint32_t kNoRouterSlot = 0xffffffffU;
constexpr std::size_t kNonceHexChars = 32U;
constexpr std::size_t kSha256HexChars = 64U;
constexpr std::size_t kSourceRevisionHexChars = 40U;
constexpr std::size_t kProducerChars = 31U;
constexpr std::size_t kModeChars = 47U;
constexpr std::size_t kReceiptBufferBytes = 128U * 1024U;

constexpr char kReceiptMagic[] = "RTS_LOCKSTEP_V2_RECEIPT";
constexpr char kReceiptProducer[] = "installed-lockstep-v2";
constexpr char kReceiptMode[] = "installed-lockstep-v2-production";

enum class CommandOriginMode : std::uint32_t
{
	DirectAuthenticated = 1U,
	TrustedRouter = 2U
};

struct SessionContract
{
	std::uint32_t schemaVersion = kSchemaVersion;
	std::uint32_t protocolEpoch = kProtocolEpoch;
	std::uint32_t localSlot = 0U;
	std::uint32_t peerCount = 0U;
	std::uint32_t rosterMask = 0U;
	std::uint32_t buildCompatibilityCrc = 0U;
	std::uint32_t contentCrc = 0U;
	std::uint32_t mapCrc = 0U;
	std::uint32_t commonStopFrame = kCommonStopFrame;
	std::uint32_t provenKernelMask = 0U;
	std::uint32_t packetRouterSlot = kNoRouterSlot;
	CommandOriginMode originMode = CommandOriginMode::DirectAuthenticated;
	std::array<char, kNonceHexChars + 1U> runNonce = {{}};
	std::array<char, kNonceHexChars + 1U> sessionNonce = {{}};
	std::array<char, kSha256HexChars + 1U> executableSha256 = {{}};
	std::array<char, kSourceRevisionHexChars + 1U> sourceRevision = {{}};
};

struct PeerCommandContribution
{
	std::uint32_t commandCount = 0U;
	std::uint32_t firstCommandFrame = 0U;
	std::uint32_t lastCommandFrame = 0U;
	std::uint16_t lastCommandId = 0U;
	bool hasLastCommandId = false;
	std::uint64_t lastCommandDigest = 0U;
	std::uint64_t commandDigest = 0U;
};

struct FrameCheckpoint
{
	std::uint32_t frame = 0U;
	std::uint32_t crc = 0U;
	std::uint64_t commandDigest = 0U;
};

// Worker telemetry is executable-origin evidence, not a negotiated request.
// The qualification process fills one record for every claimed kernel bit
// after observing actual physical worker executions.  A peer may never fill a
// mask from the session contract alone.
struct KernelWorkerTelemetry
{
	std::uint64_t physicalWorkerMask = 0U;
	std::uint32_t physicalWorkerJobs = 0U;
	std::uint32_t distinctPhysicalWorkers = 0U;
	std::uint32_t peakConcurrentPhysicalWorkers = 0U;
	bool physicalWorkerMaskComplete = false;
};

struct WorkerTelemetry
{
	std::uint32_t authorityMask = 0U;
	bool executableOrigin = false;
	std::array<KernelWorkerTelemetry, kKernelCount> kernels = {{}};
};

struct Receipt
{
	SessionContract session;
	std::uint64_t networkSessionToken = 0U;
	std::uint32_t finalFrame = 0U;
	std::uint32_t frameCount = 0U;
	std::uint32_t contributedPeerMask = 0U;
	std::uint32_t checkpointCount = 0U;
	std::uint32_t validationAuthorityMask = 0U;
	bool executableOrigin = false;
	bool workerTelemetryExecutableOrigin = false;
	bool transportPathUsed = false;
	bool handshakeValidated = false;
	bool cleanShutdown = false;
	std::array<PeerCommandContribution, kMaxPeerCount> contributions = {{}};
	std::array<KernelWorkerTelemetry, kKernelCount> workerTelemetry = {{}};
	std::array<FrameCheckpoint, kMaxCheckpoints> checkpoints = {{}};
};

enum class ValidationError : std::uint8_t
{
	None = 0U,
	NullInput,
	InvalidSchema,
	InvalidProtocolEpoch,
	InvalidMode,
	InvalidNonce,
	InvalidExecutableHash,
	InvalidSourceRevision,
	InvalidRoster,
	InvalidFrame,
	InvalidNetworkSession,
	MissingExecutableOrigin,
	MissingTransportPath,
	MissingHandshake,
	UncleanShutdown,
	MissingPeerContribution,
	MissingCheckpoint,
	NonMonotonicCheckpoint,
	InvalidReceiptText,
	OutputTooSmall,
	AuthorityNotProven,
};

struct ValidationResult
{
	ValidationError error = ValidationError::None;

	constexpr bool ok() const
	{
		return error == ValidationError::None;
	}
};

// This is a deterministic digest for receipt equality, not an authenticator.
// Origin authenticity in v2 comes from the direct topology (or a separate
// authenticated envelope); callers must not treat this digest as a MAC.
std::uint64_t ComputeCommandDigest(const runtime_epoch::Byte *bytes,
	std::size_t byteCount);
std::uint64_t MixCommandDigest(std::uint64_t priorDigest,
	std::uint32_t frame,
	std::uint32_t originSlot,
	std::uint16_t commandId,
	std::uint64_t commandDigest);

bool IsCanonicalHex(const char *value, std::size_t hexChars);
bool IsValidSessionContract(const SessionContract &session);

ValidationResult ValidateReceipt(const Receipt &receipt,
	const SessionContract &expectedSession,
	std::uint64_t expectedNetworkSessionToken,
	std::uint32_t expectedPeerMask,
	bool allowTrustedRouter,
	std::uint32_t actualValidationAuthorityMask);

// Resolve only from a fully validated executable-origin receipt.  This
// function intentionally returns zero for every failure, including a caller
// supplied nonzero requested mask, so an invalid receipt cannot grant worker
// authority to an ordinary product build.
std::uint32_t ResolveValidatedKernelMask(const Receipt &receipt,
	const SessionContract &expectedSession,
	std::uint64_t expectedNetworkSessionToken,
	std::uint32_t expectedPeerMask,
	bool allowTrustedRouter,
	std::uint32_t requestedMask,
	std::uint32_t actualValidationAuthorityMask);

// The text form is a canonical, bounded interchange format for one process's
// receipt.  It is intentionally independent from the v1 evidence manifest.
bool EncodeReceipt(const Receipt &receipt,
	char *output,
	std::size_t outputCapacity,
	std::size_t *written);
ValidationResult DecodeReceipt(const char *input,
	std::size_t inputSize,
	Receipt *receipt);

class ReceiptRecorder
{
public:
	ReceiptRecorder();

	void reset();
	bool begin(const SessionContract &session,
		std::uint64_t networkSessionToken,
		const char *actualExecutableSha256,
		const WorkerTelemetry &workerTelemetry);
	// Installed qualification begins recording before the candidate kernels run.
	// It may publish executable-origin telemetry exactly once at the common stop
	// boundary; finish remains fail-closed until that telemetry validates.
	bool beginQualification(const SessionContract &session,
		std::uint64_t networkSessionToken,
		const char *actualExecutableSha256);
	bool publishWorkerTelemetry(const WorkerTelemetry &workerTelemetry);
	bool recordCommand(std::uint32_t frame,
		std::uint32_t originSlot,
		std::uint16_t commandId,
		std::uint64_t commandDigest);
	bool recordFrame(std::uint32_t frame,
		std::uint32_t crc,
		std::uint64_t commandDigest);
	bool finish(bool cleanShutdown,
		bool transportPathUsed,
		bool handshakeValidated);

	bool isActive() const { return m_active; }
	bool hasFailure() const { return m_failed; }
	const Receipt &receipt() const { return m_receipt; }

private:
	Receipt m_receipt;
	std::uint32_t m_nextFrame;
	bool m_active;
	bool m_failed;
};

} // namespace lockstep_v2
} // namespace rts
