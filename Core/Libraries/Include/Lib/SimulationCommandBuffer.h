/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once

#include "Lib/BaseTypeCore.h"

namespace rts
{
// Pointer-free object identity copied by the simulation owner. Zero denotes
// the global/no-object handle. Instances expose no field-level mutation.
class SimulationStableHandle
{
public:
	SimulationStableHandle();
	explicit SimulationStableHandle(UnsignedInt objectID);
	UnsignedInt objectID() const;
	bool isNull() const;
private:
	UnsignedInt m_objectID;
};

// Canonical ordering is phase, target ObjectID, source ObjectID, stable module
// type, then producer sequence. Module type must be a build-stable adapter ID,
// never a runtime NameKey. Producer sequence packs producer ordinal and the
// local emission index into one immutable value.
class SimulationCommandOrderKey
{
public:
	SimulationCommandOrderKey();
	SimulationCommandOrderKey(UnsignedInt phase,
		const SimulationStableHandle &target,
		const SimulationStableHandle &source,
		UnsignedInt moduleType, UnsignedInt64 producerSequence);
	UnsignedInt phase() const;
	const SimulationStableHandle &target() const;
	const SimulationStableHandle &source() const;
	UnsignedInt moduleType() const;
	UnsignedInt64 producerSequence() const;
	bool isValid() const;
private:
	UnsignedInt m_phase;
	SimulationStableHandle m_target;
	SimulationStableHandle m_source;
	UnsignedInt m_moduleType;
	UnsignedInt64 m_producerSequence;
};

UnsignedInt64 MakeSimulationProducerSequence(UnsignedInt producerOrdinal,
	UnsignedInt localEmissionIndex);
UnsignedInt SimulationProducerOrdinal(UnsignedInt64 producerSequence);
UnsignedInt SimulationProducerLocalIndex(UnsignedInt64 producerSequence);
int CompareSimulationCommandOrderKeys(const SimulationCommandOrderKey &left,
	const SimulationCommandOrderKey &right);

class SimulationCommand
{
public:
	SimulationCommand();
	const SimulationCommandOrderKey &orderKey() const;
	UnsignedInt commandType() const;
	UnsignedInt payloadOffset() const;
	UnsignedInt payloadSize() const;
private:
	friend class SimulationCommandBuffer;
	SimulationCommand(const SimulationCommandOrderKey &orderKey,
		UnsignedInt commandType, UnsignedInt payloadOffset,
		UnsignedInt payloadSize);
	SimulationCommandOrderKey m_orderKey;
	UnsignedInt m_commandType;
	UnsignedInt m_payloadOffset;
	UnsignedInt m_payloadSize;
};

enum SimulationCommandBufferStatus
{
	SIMULATION_COMMAND_BUFFER_WRITING = 0,
	SIMULATION_COMMAND_BUFFER_COMPLETE,
	SIMULATION_COMMAND_BUFFER_PRODUCER_FAULT,
	SIMULATION_COMMAND_BUFFER_COMMAND_OVERFLOW,
	SIMULATION_COMMAND_BUFFER_PAYLOAD_OVERFLOW,
	SIMULATION_COMMAND_BUFFER_INVALID_COMMAND,
	SIMULATION_COMMAND_BUFFER_INVALID_STORAGE
};

// One producer owns one instance and its caller-provided storage for a wave.
// append, complete, and fail allocate no memory and touch no shared slot.
class SimulationCommandBuffer
{
public:
	SimulationCommandBuffer(SimulationCommand *commandStorage,
		UnsignedInt commandCapacity, unsigned char *payloadStorage,
		UnsignedInt payloadCapacity, UnsignedInt producerOrdinal,
		UnsignedInt moduleType);
	void reset();
	bool append(UnsignedInt phase, const SimulationStableHandle &target,
		const SimulationStableHandle &source, UnsignedInt commandType,
		const void *payload, UnsignedInt payloadSize);
	// Reserves disjoint producer-owned payload bytes for direct worker fill.
	// The command remains unpublished until complete() succeeds.
	bool appendReserved(UnsignedInt phase,
		const SimulationStableHandle &target,
		const SimulationStableHandle &source, UnsignedInt commandType,
		UnsignedInt payloadSize, unsigned char **payload);
	bool complete();
	void fail();
	SimulationCommandBufferStatus status() const;
	UnsignedInt commandCount() const;
	UnsignedInt commandCapacity() const;
	UnsignedInt payloadByteCount() const;
	UnsignedInt payloadCapacity() const;
	UnsignedInt producerOrdinal() const;
	UnsignedInt moduleType() const;
	const SimulationCommand *commandAt(UnsignedInt index) const;
	const unsigned char *payloadFor(const SimulationCommand &command) const;
private:
	SimulationCommandBuffer(const SimulationCommandBuffer &);
	SimulationCommandBuffer &operator=(const SimulationCommandBuffer &);
	SimulationCommand *m_commands;
	unsigned char *m_payload;
	UnsignedInt m_commandCapacity;
	UnsignedInt m_payloadCapacity;
	UnsignedInt m_commandCount;
	UnsignedInt m_payloadByteCount;
	UnsignedInt m_producerOrdinal;
	UnsignedInt m_moduleType;
	SimulationCommandBufferStatus m_status;
};

enum SimulationCommandValidationStatus
{
	SIMULATION_COMMANDS_VALID = 0,
	SIMULATION_COMMANDS_INVALID_ARGUMENT,
	SIMULATION_COMMANDS_PRODUCER_INCOMPLETE,
	SIMULATION_COMMANDS_PRODUCER_FAULT,
	SIMULATION_COMMANDS_COMMAND_OVERFLOW,
	SIMULATION_COMMANDS_PAYLOAD_OVERFLOW,
	SIMULATION_COMMANDS_INVALID_COMMAND,
	SIMULATION_COMMANDS_INVALID_STORAGE,
	SIMULATION_COMMANDS_OUTPUT_OVERFLOW,
	SIMULATION_COMMANDS_DUPLICATE_KEY
};

const UnsignedInt SIMULATION_INVALID_COMMAND_INDEX =
	static_cast<UnsignedInt>(-1);

SimulationCommandValidationStatus ValidateSimulationCommandBuffer(
	const SimulationCommandBuffer &buffer, UnsignedInt *invalidCommandIndex);

// An immutable view into one producer's still-live fixed storage.
class SimulationMergedCommand
{
public:
	SimulationMergedCommand();
	const SimulationCommand *command() const;
	const unsigned char *payload() const;
	UnsignedInt producerSlot() const;
	UnsignedInt producerCommandIndex() const;
private:
	friend struct SimulationCommandMergeAccess;
	const SimulationCommand *m_command;
	const unsigned char *m_payload;
	UnsignedInt m_producerSlot;
	UnsignedInt m_producerCommandIndex;
};

struct SimulationCommandMergeResult
{
	SimulationCommandMergeResult();
	SimulationCommandValidationStatus status;
	UnsignedInt commandCount;
	UnsignedInt producerSlot;
	UnsignedInt producerCommandIndex;
	bool succeeded() const;
};

// producerSlots is indexed by owner-captured producer order, never job
// completion order. The stable merge uses only caller-provided output/scratch
// arrays. Any failure returns commandCount == 0, so the owner cannot partially
// apply a failed wave.
SimulationCommandMergeResult MergeSimulationCommandSlots(
	const SimulationCommandBuffer *const *producerSlots,
	UnsignedInt producerSlotCount, SimulationMergedCommand *output,
	SimulationMergedCommand *scratch, UnsignedInt outputCapacity);
}
