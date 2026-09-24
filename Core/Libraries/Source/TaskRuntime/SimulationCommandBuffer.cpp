/*
** Command & Conquer Generals Zero Hour(tm)
** Copyright 2026 TheSuperHackers
** SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "Lib/SimulationCommandBuffer.h"

#include <limits.h>
#include <string.h>

namespace rts
{
SimulationStableHandle::SimulationStableHandle() : m_objectID(0) {}

SimulationStableHandle::SimulationStableHandle(UnsignedInt objectID)
	: m_objectID(objectID) {}

UnsignedInt SimulationStableHandle::objectID() const { return m_objectID; }
bool SimulationStableHandle::isNull() const { return m_objectID == 0; }

SimulationCommandOrderKey::SimulationCommandOrderKey()
	: m_phase(0), m_target(), m_source(), m_moduleType(0),
	  m_producerSequence(0) {}

SimulationCommandOrderKey::SimulationCommandOrderKey(UnsignedInt phase,
	const SimulationStableHandle &target,
	const SimulationStableHandle &source, UnsignedInt moduleType,
	UnsignedInt64 producerSequence)
	: m_phase(phase), m_target(target), m_source(source),
	  m_moduleType(moduleType), m_producerSequence(producerSequence) {}

UnsignedInt SimulationCommandOrderKey::phase() const { return m_phase; }
const SimulationStableHandle &SimulationCommandOrderKey::target() const
{
	return m_target;
}
const SimulationStableHandle &SimulationCommandOrderKey::source() const
{
	return m_source;
}
UnsignedInt SimulationCommandOrderKey::moduleType() const
{
	return m_moduleType;
}
UnsignedInt64 SimulationCommandOrderKey::producerSequence() const
{
	return m_producerSequence;
}
bool SimulationCommandOrderKey::isValid() const { return m_moduleType != 0; }

UnsignedInt64 MakeSimulationProducerSequence(UnsignedInt producerOrdinal,
	UnsignedInt localEmissionIndex)
{
	return (static_cast<UnsignedInt64>(producerOrdinal) << 32) |
		static_cast<UnsignedInt64>(localEmissionIndex);
}

UnsignedInt SimulationProducerOrdinal(UnsignedInt64 producerSequence)
{
	return static_cast<UnsignedInt>(producerSequence >> 32);
}

UnsignedInt SimulationProducerLocalIndex(UnsignedInt64 producerSequence)
{
	return static_cast<UnsignedInt>(producerSequence &
		static_cast<UnsignedInt64>(UINT_MAX));
}

int CompareSimulationCommandOrderKeys(const SimulationCommandOrderKey &left,
	const SimulationCommandOrderKey &right)
{
	if (left.phase() != right.phase())
		return left.phase() < right.phase() ? -1 : 1;
	if (left.target().objectID() != right.target().objectID())
		return left.target().objectID() < right.target().objectID() ? -1 : 1;
	if (left.source().objectID() != right.source().objectID())
		return left.source().objectID() < right.source().objectID() ? -1 : 1;
	if (left.moduleType() != right.moduleType())
		return left.moduleType() < right.moduleType() ? -1 : 1;
	if (left.producerSequence() != right.producerSequence())
		return left.producerSequence() < right.producerSequence() ? -1 : 1;
	return 0;
}

SimulationCommand::SimulationCommand()
	: m_orderKey(), m_commandType(0), m_payloadOffset(0), m_payloadSize(0) {}

SimulationCommand::SimulationCommand(
	const SimulationCommandOrderKey &orderKey, UnsignedInt commandType,
	UnsignedInt payloadOffset, UnsignedInt payloadSize)
	: m_orderKey(orderKey), m_commandType(commandType),
	  m_payloadOffset(payloadOffset), m_payloadSize(payloadSize) {}

const SimulationCommandOrderKey &SimulationCommand::orderKey() const
{
	return m_orderKey;
}
UnsignedInt SimulationCommand::commandType() const { return m_commandType; }
UnsignedInt SimulationCommand::payloadOffset() const { return m_payloadOffset; }
UnsignedInt SimulationCommand::payloadSize() const { return m_payloadSize; }

SimulationCommandBuffer::SimulationCommandBuffer(
	SimulationCommand *commandStorage, UnsignedInt commandCapacity,
	unsigned char *payloadStorage, UnsignedInt payloadCapacity,
	UnsignedInt producerOrdinal, UnsignedInt moduleType)
	: m_commands(commandStorage), m_payload(payloadStorage),
	  m_commandCapacity(commandCapacity), m_payloadCapacity(payloadCapacity),
	  m_commandCount(0), m_payloadByteCount(0),
	  m_producerOrdinal(producerOrdinal), m_moduleType(moduleType),
	  m_status(SIMULATION_COMMAND_BUFFER_WRITING)
{
	if ((m_commandCapacity != 0 && m_commands == 0) ||
		(m_payloadCapacity != 0 && m_payload == 0) || m_moduleType == 0)
	{
		m_status = SIMULATION_COMMAND_BUFFER_INVALID_STORAGE;
	}
}

void SimulationCommandBuffer::reset()
{
	m_commandCount = 0;
	m_payloadByteCount = 0;
	m_status = SIMULATION_COMMAND_BUFFER_WRITING;
	if ((m_commandCapacity != 0 && m_commands == 0) ||
		(m_payloadCapacity != 0 && m_payload == 0) || m_moduleType == 0)
	{
		m_status = SIMULATION_COMMAND_BUFFER_INVALID_STORAGE;
	}
}

bool SimulationCommandBuffer::append(UnsignedInt phase,
	const SimulationStableHandle &target,
	const SimulationStableHandle &source, UnsignedInt commandType,
	const void *payload, UnsignedInt payloadSize)
{
	unsigned char *reserved = 0;
	if (payloadSize != 0 && payload == 0)
	{
		m_status = SIMULATION_COMMAND_BUFFER_INVALID_COMMAND;
		return false;
	}
	if (!appendReserved(phase, target, source, commandType, payloadSize,
		&reserved))
		return false;
	if (payloadSize != 0) memcpy(reserved, payload, payloadSize);
	return true;
}

bool SimulationCommandBuffer::appendReserved(UnsignedInt phase,
	const SimulationStableHandle &target,
	const SimulationStableHandle &source, UnsignedInt commandType,
	UnsignedInt payloadSize, unsigned char **payload)
{
	UnsignedInt payloadOffset;
	SimulationCommandOrderKey orderKey;

	if (payload != 0) *payload = 0;
	if (m_status != SIMULATION_COMMAND_BUFFER_WRITING) return false;
	if (commandType == 0 || (payloadSize != 0 && payload == 0))
	{
		m_status = SIMULATION_COMMAND_BUFFER_INVALID_COMMAND;
		return false;
	}
	if (m_commandCount >= m_commandCapacity)
	{
		m_status = SIMULATION_COMMAND_BUFFER_COMMAND_OVERFLOW;
		return false;
	}
	if (payloadSize > m_payloadCapacity - m_payloadByteCount)
	{
		m_status = SIMULATION_COMMAND_BUFFER_PAYLOAD_OVERFLOW;
		return false;
	}

	payloadOffset = m_payloadByteCount;
	if (payloadSize != 0)
	{
		*payload = m_payload + payloadOffset;
		m_payloadByteCount += payloadSize;
	}
	orderKey = SimulationCommandOrderKey(phase, target, source, m_moduleType,
		MakeSimulationProducerSequence(m_producerOrdinal, m_commandCount));
	m_commands[m_commandCount] = SimulationCommand(orderKey, commandType,
		payloadOffset, payloadSize);
	++m_commandCount;
	return true;
}

bool SimulationCommandBuffer::complete()
{
	if (m_status != SIMULATION_COMMAND_BUFFER_WRITING) return false;
	m_status = SIMULATION_COMMAND_BUFFER_COMPLETE;
	return true;
}

void SimulationCommandBuffer::fail()
{
	if (m_status == SIMULATION_COMMAND_BUFFER_WRITING)
		m_status = SIMULATION_COMMAND_BUFFER_PRODUCER_FAULT;
}

SimulationCommandBufferStatus SimulationCommandBuffer::status() const
{
	return m_status;
}
UnsignedInt SimulationCommandBuffer::commandCount() const
{
	return m_commandCount;
}
UnsignedInt SimulationCommandBuffer::commandCapacity() const
{
	return m_commandCapacity;
}
UnsignedInt SimulationCommandBuffer::payloadByteCount() const
{
	return m_payloadByteCount;
}
UnsignedInt SimulationCommandBuffer::payloadCapacity() const
{
	return m_payloadCapacity;
}
UnsignedInt SimulationCommandBuffer::producerOrdinal() const
{
	return m_producerOrdinal;
}
UnsignedInt SimulationCommandBuffer::moduleType() const { return m_moduleType; }

const SimulationCommand *SimulationCommandBuffer::commandAt(
	UnsignedInt index) const
{
	return index < m_commandCount ? m_commands + index : 0;
}

const unsigned char *SimulationCommandBuffer::payloadFor(
	const SimulationCommand &command) const
{
	if (command.payloadSize() == 0) return 0;
	if (command.payloadOffset() > m_payloadByteCount ||
		command.payloadSize() > m_payloadByteCount - command.payloadOffset())
	{
		return 0;
	}
	return m_payload + command.payloadOffset();
}

static SimulationCommandValidationStatus ValidationForBufferStatus(
	SimulationCommandBufferStatus status)
{
	switch (status)
	{
	case SIMULATION_COMMAND_BUFFER_COMPLETE:
		return SIMULATION_COMMANDS_VALID;
	case SIMULATION_COMMAND_BUFFER_WRITING:
		return SIMULATION_COMMANDS_PRODUCER_INCOMPLETE;
	case SIMULATION_COMMAND_BUFFER_PRODUCER_FAULT:
		return SIMULATION_COMMANDS_PRODUCER_FAULT;
	case SIMULATION_COMMAND_BUFFER_COMMAND_OVERFLOW:
		return SIMULATION_COMMANDS_COMMAND_OVERFLOW;
	case SIMULATION_COMMAND_BUFFER_PAYLOAD_OVERFLOW:
		return SIMULATION_COMMANDS_PAYLOAD_OVERFLOW;
	case SIMULATION_COMMAND_BUFFER_INVALID_COMMAND:
		return SIMULATION_COMMANDS_INVALID_COMMAND;
	case SIMULATION_COMMAND_BUFFER_INVALID_STORAGE:
		return SIMULATION_COMMANDS_INVALID_STORAGE;
	default:
		return SIMULATION_COMMANDS_INVALID_COMMAND;
	}
}

SimulationCommandValidationStatus ValidateSimulationCommandBuffer(
	const SimulationCommandBuffer &buffer, UnsignedInt *invalidCommandIndex)
{
	SimulationCommandValidationStatus status =
		ValidationForBufferStatus(buffer.status());
	UnsignedInt index;
	if (invalidCommandIndex != 0)
		*invalidCommandIndex = SIMULATION_INVALID_COMMAND_INDEX;
	if (status != SIMULATION_COMMANDS_VALID) return status;
	if (buffer.commandCount() > buffer.commandCapacity() ||
		buffer.payloadByteCount() > buffer.payloadCapacity())
	{
		return SIMULATION_COMMANDS_INVALID_STORAGE;
	}

	for (index = 0; index < buffer.commandCount(); ++index)
	{
		const SimulationCommand *command = buffer.commandAt(index);
		if (command == 0 || command->commandType() == 0 ||
			!command->orderKey().isValid() ||
			command->orderKey().moduleType() != buffer.moduleType() ||
			command->orderKey().producerSequence() !=
				MakeSimulationProducerSequence(buffer.producerOrdinal(), index) ||
			command->payloadOffset() > buffer.payloadByteCount() ||
			command->payloadSize() >
				buffer.payloadByteCount() - command->payloadOffset())
		{
			if (invalidCommandIndex != 0) *invalidCommandIndex = index;
			return SIMULATION_COMMANDS_INVALID_COMMAND;
		}
	}
	return SIMULATION_COMMANDS_VALID;
}

SimulationMergedCommand::SimulationMergedCommand()
	: m_command(0), m_payload(0), m_producerSlot(0),
	  m_producerCommandIndex(0) {}

const SimulationCommand *SimulationMergedCommand::command() const
{
	return m_command;
}
const unsigned char *SimulationMergedCommand::payload() const
{
	return m_payload;
}
UnsignedInt SimulationMergedCommand::producerSlot() const
{
	return m_producerSlot;
}
UnsignedInt SimulationMergedCommand::producerCommandIndex() const
{
	return m_producerCommandIndex;
}

SimulationCommandMergeResult::SimulationCommandMergeResult()
	: status(SIMULATION_COMMANDS_VALID), commandCount(0),
	  producerSlot(SIMULATION_INVALID_COMMAND_INDEX),
	  producerCommandIndex(SIMULATION_INVALID_COMMAND_INDEX) {}

bool SimulationCommandMergeResult::succeeded() const
{
	return status == SIMULATION_COMMANDS_VALID;
}

struct SimulationCommandMergeAccess
{
	static void Set(SimulationMergedCommand &merged,
		const SimulationCommand *command, const unsigned char *payload,
		UnsignedInt producerSlot, UnsignedInt producerCommandIndex)
	{
		merged.m_command = command;
		merged.m_payload = payload;
		merged.m_producerSlot = producerSlot;
		merged.m_producerCommandIndex = producerCommandIndex;
	}
};

static void StableSortMergedCommands(SimulationMergedCommand *output,
	SimulationMergedCommand *scratch, UnsignedInt count)
{
	SimulationMergedCommand *source = output;
	SimulationMergedCommand *destination = scratch;
	UnsignedInt width = 1;

	while (width < count)
	{
		UnsignedInt left = 0;
		while (left < count)
		{
			UnsignedInt middle = left +
				(width < count - left ? width : count - left);
			UnsignedInt right = middle +
				(width < count - middle ? width : count - middle);
			UnsignedInt first = left;
			UnsignedInt second = middle;
			UnsignedInt write = left;

			while (first < middle && second < right)
			{
				if (CompareSimulationCommandOrderKeys(
					source[first].command()->orderKey(),
					source[second].command()->orderKey()) <= 0)
				{
					destination[write++] = source[first++];
				}
				else
				{
					destination[write++] = source[second++];
				}
			}
			while (first < middle) destination[write++] = source[first++];
			while (second < right) destination[write++] = source[second++];
			left = right;
		}

		SimulationMergedCommand *temporary = source;
		source = destination;
		destination = temporary;
		if (width > count / 2) width = count;
		else width *= 2;
	}

	if (source != output)
	{
		UnsignedInt index;
		for (index = 0; index < count; ++index) output[index] = source[index];
	}
}

SimulationCommandMergeResult MergeSimulationCommandSlots(
	const SimulationCommandBuffer *const *producerSlots,
	UnsignedInt producerSlotCount, SimulationMergedCommand *output,
	SimulationMergedCommand *scratch, UnsignedInt outputCapacity)
{
	SimulationCommandMergeResult result;
	UnsignedInt total = 0;
	UnsignedInt slot;

	if (producerSlotCount != 0 && producerSlots == 0)
	{
		result.status = SIMULATION_COMMANDS_INVALID_ARGUMENT;
		return result;
	}

	// Validate every fixed slot and total capacity before writing output.
	for (slot = 0; slot < producerSlotCount; ++slot)
	{
		UnsignedInt invalidCommand = SIMULATION_INVALID_COMMAND_INDEX;
		SimulationCommandValidationStatus status;
		if (producerSlots[slot] == 0)
		{
			result.status = SIMULATION_COMMANDS_INVALID_ARGUMENT;
			result.producerSlot = slot;
			return result;
		}
		status = ValidateSimulationCommandBuffer(*producerSlots[slot],
			&invalidCommand);
		if (status != SIMULATION_COMMANDS_VALID)
		{
			result.status = status;
			result.producerSlot = slot;
			result.producerCommandIndex = invalidCommand;
			return result;
		}
		if (producerSlots[slot]->commandCount() > UINT_MAX - total)
		{
			result.status = SIMULATION_COMMANDS_OUTPUT_OVERFLOW;
			result.producerSlot = slot;
			return result;
		}
		total += producerSlots[slot]->commandCount();
	}

	if (total > outputCapacity || (total != 0 && output == 0) ||
		(total > 1 && (scratch == 0 || scratch == output)))
	{
		result.status = SIMULATION_COMMANDS_OUTPUT_OVERFLOW;
		return result;
	}

	total = 0;
	for (slot = 0; slot < producerSlotCount; ++slot)
	{
		UnsignedInt commandIndex;
		for (commandIndex = 0;
			commandIndex < producerSlots[slot]->commandCount(); ++commandIndex)
		{
			const SimulationCommand *command =
				producerSlots[slot]->commandAt(commandIndex);
			SimulationCommandMergeAccess::Set(output[total++], command,
				producerSlots[slot]->payloadFor(*command), slot, commandIndex);
		}
	}

	if (total > 1) StableSortMergedCommands(output, scratch, total);
	for (slot = 1; slot < total; ++slot)
	{
		if (CompareSimulationCommandOrderKeys(
			output[slot - 1].command()->orderKey(),
			output[slot].command()->orderKey()) == 0)
		{
			result.status = SIMULATION_COMMANDS_DUPLICATE_KEY;
			result.producerSlot = output[slot].producerSlot();
			result.producerCommandIndex =
				output[slot].producerCommandIndex();
			return result;
		}
	}

	result.commandCount = total;
	return result;
}
}
