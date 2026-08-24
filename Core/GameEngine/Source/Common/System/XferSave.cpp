/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: XferSave.cpp /////////////////////////////////////////////////////////////////////////////
// Author: Colin Day, February 2002
// Desc:   Xfer disk write implementation
///////////////////////////////////////////////////////////////////////////////////////////////////

// USER INCLUDES //////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include "Common/XferSave.h"
#include "Common/Snapshot.h"
#include "Common/GameMemory.h"

#ifdef _WIN64
#include "Lib/RuntimeEpochContract.h"

#include <array>
#include <limits>
#endif

// PRIVATE TYPES //////////////////////////////////////////////////////////////////////////////////
class XferBlockData : public MemoryPoolObject
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE(XferBlockData, "XferBlockData")

public:

	XferFilePos filePos;			///< the file position of this block
	XferBlockData *next;			///< next block on the stack

};
EMPTY_DTOR(XferBlockData)

///////////////////////////////////////////////////////////////////////////////////////////////////
// PUBLIC METHDOS /////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
XferSave::XferSave()
{

	m_xferMode = XFER_SAVE;
	m_fileFP = nullptr;
	m_blockStack = nullptr;

#ifdef _WIN64
	m_runtimeEpochExecutableCrc = 0U;
	m_runtimeEpochIniCrc = 0U;
	m_runtimeEpochIdentityConfigured = FALSE;
	m_runtimeEpochFinalized = FALSE;
#endif

}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
XferSave::~XferSave()
{

	// warn the user if a file was left open
	if( m_fileFP != nullptr )
	{

		DEBUG_CRASH(( "Warning: Xfer file '%s' was left open", m_identifier.str() ));
		close();

	}

	//
	// the block stack should be empty, if it's not that means we started blocks but never
	// called enough matching end blocks
	//
	if( m_blockStack != nullptr )
	{

		// tell the user there is an error
		DEBUG_CRASH(( "Warning: XferSave::~XferSave - m_blockStack was not null!" ));

		// delete the block stack
		XferBlockData *next;
		while( m_blockStack )
		{

			next = m_blockStack->next;
			deleteInstance(m_blockStack);
			m_blockStack = next;

		}

	}

}

//-------------------------------------------------------------------------------------------------
/** Open file 'identifier' for writing */
//-------------------------------------------------------------------------------------------------
void XferSave::open( AsciiString identifier )
{

	// sanity, check to see if we're already open
	if( m_fileFP != nullptr )
	{

		DEBUG_CRASH(( "Cannot open file '%s' cause we've already got '%s' open",
									identifier.str(), m_identifier.str() ));
		throw XFER_FILE_ALREADY_OPEN;

	}

	// call base class
	Xfer::open( identifier );

	// open the file
	m_fileFP = fopen( identifier.str(), "w+b" );
	if( m_fileFP == nullptr )
	{

		DEBUG_CRASH(( "File '%s' not found", identifier.str() ));
		throw XFER_FILE_NOT_FOUND;

	}

#ifdef _WIN64
	if( m_runtimeEpochIdentityConfigured == FALSE )
	{
		fclose( m_fileFP );
		m_fileFP = nullptr;
		m_identifier.clear();
		DEBUG_CRASH(( "XferSave::open - Native x64 identity was not configured" ));
		throw XFER_INVALID_PARAMETERS;
	}

	// Reserve the fixed header immediately.  A save that exits before
	// finalizeRuntimeEpoch() therefore retains an intentionally impossible
	// payload length and cannot be mistaken for a complete file.
	rts::runtime_epoch::SaveHeader header;
	header.buildCompatibilityId = rts::runtime_epoch::BuildCompatibilityIdFromExecutableCrc(
		m_runtimeEpochExecutableCrc);
	header.contentHash = rts::runtime_epoch::ContentHashFromIniCrc(m_runtimeEpochIniCrc);
	header.payloadByteCount = std::numeric_limits<std::uint64_t>::max();
	header.payloadChecksum = 0U;
	const std::array<rts::runtime_epoch::Byte, rts::runtime_epoch::kHeaderSize> encoded =
		rts::runtime_epoch::Encode(header);
	if( fwrite( encoded.data(), 1, encoded.size(), m_fileFP ) != encoded.size() )
	{
		fclose( m_fileFP );
		m_fileFP = nullptr;
		m_identifier.clear();
		DEBUG_CRASH(( "XferSave::open - Error writing native x64 save header" ));
		throw XFER_WRITE_ERROR;
	}
	m_runtimeEpochFinalized = FALSE;
#endif

}

#ifdef _WIN64
void XferSave::setRuntimeEpochIdentity( std::uint32_t executableCrc, std::uint32_t iniCrc )
{
	if( m_fileFP != nullptr )
	{
		DEBUG_CRASH(( "XferSave::setRuntimeEpochIdentity - file is already open" ));
		throw XFER_FILE_ALREADY_OPEN;
	}

	m_runtimeEpochExecutableCrc = executableCrc;
	m_runtimeEpochIniCrc = iniCrc;
	m_runtimeEpochIdentityConfigured = TRUE;
}

void XferSave::finalizeRuntimeEpoch()
{
	if( m_fileFP == nullptr )
	{
		DEBUG_CRASH(( "XferSave::finalizeRuntimeEpoch - file is not open" ));
		throw XFER_FILE_NOT_OPEN;
	}
	if( m_runtimeEpochIdentityConfigured == FALSE )
	{
		DEBUG_CRASH(( "XferSave::finalizeRuntimeEpoch - identity was not configured" ));
		throw XFER_INVALID_PARAMETERS;
	}
	if( m_runtimeEpochFinalized != FALSE )
	{
		DEBUG_CRASH(( "XferSave::finalizeRuntimeEpoch - file was already finalized" ));
		throw XFER_INVALID_PARAMETERS;
	}
	if( m_blockStack != nullptr )
	{
		DEBUG_CRASH(( "XferSave::finalizeRuntimeEpoch - an Xfer block is still open" ));
		throw XFER_BEGIN_END_MISMATCH;
	}
	if( fflush( m_fileFP ) != 0 )
	{
		DEBUG_CRASH(( "XferSave::finalizeRuntimeEpoch - flush failed" ));
		throw XFER_WRITE_ERROR;
	}

	const __int64 fileEnd = _ftelli64( m_fileFP );
	if( fileEnd < static_cast<__int64>(rts::runtime_epoch::kHeaderSize) )
	{
		DEBUG_CRASH(( "XferSave::finalizeRuntimeEpoch - invalid file position" ));
		throw XFER_WRITE_ERROR;
	}

	const std::uint64_t payloadSize = static_cast<std::uint64_t>(
		fileEnd - static_cast<__int64>(rts::runtime_epoch::kHeaderSize));
	if( _fseeki64( m_fileFP, static_cast<__int64>(rts::runtime_epoch::kHeaderSize), SEEK_SET ) != 0 )
	{
		DEBUG_CRASH(( "XferSave::finalizeRuntimeEpoch - unable to seek to payload" ));
		throw XFER_WRITE_ERROR;
	}

	rts::runtime_epoch::PayloadChecksumAccumulator checksum;
	std::array<rts::runtime_epoch::Byte, 64U * 1024U> buffer = {{}};
	std::uint64_t remaining = payloadSize;
	while( remaining != 0U )
	{
		const std::size_t requested = static_cast<std::size_t>(
			remaining < buffer.size() ? remaining : buffer.size());
		const std::size_t read = fread( buffer.data(), 1, requested, m_fileFP );
		if( read != requested )
		{
			DEBUG_CRASH(( "XferSave::finalizeRuntimeEpoch - unable to read payload for checksum" ));
			throw XFER_READ_ERROR;
		}
		checksum.update( buffer.data(), read );
		remaining -= static_cast<std::uint64_t>(read);
	}

	rts::runtime_epoch::SaveHeader header;
	header.buildCompatibilityId = rts::runtime_epoch::BuildCompatibilityIdFromExecutableCrc(
		m_runtimeEpochExecutableCrc);
	header.contentHash = rts::runtime_epoch::ContentHashFromIniCrc(m_runtimeEpochIniCrc);
	header.payloadByteCount = checksum.byteCount();
	header.payloadChecksum = checksum.finish();
	const std::array<rts::runtime_epoch::Byte, rts::runtime_epoch::kHeaderSize> encoded =
		rts::runtime_epoch::Encode(header);

	if( _fseeki64( m_fileFP, 0, SEEK_SET ) != 0 ||
		fwrite( encoded.data(), 1, encoded.size(), m_fileFP ) != encoded.size() ||
		fflush( m_fileFP ) != 0 ||
		_fseeki64( m_fileFP, fileEnd, SEEK_SET ) != 0 )
	{
		DEBUG_CRASH(( "XferSave::finalizeRuntimeEpoch - unable to publish header" ));
		throw XFER_WRITE_ERROR;
	}

	m_runtimeEpochFinalized = TRUE;
}
#endif

//-------------------------------------------------------------------------------------------------
/** Close our current file */
//-------------------------------------------------------------------------------------------------
void XferSave::close()
{

	// sanity, if we don't have an open file we can do nothing
	if( m_fileFP == nullptr )
	{

		DEBUG_CRASH(( "Xfer close called, but no file was open" ));
		throw XFER_FILE_NOT_OPEN;

	}

	// close the file
	fclose( m_fileFP );
	m_fileFP = nullptr;

	// erase the filename
	m_identifier.clear();

}

//-------------------------------------------------------------------------------------------------
/** Write a placeholder at the current location in the file and store this location
	* internally.  The next endBlock that is called will back up to the most recently stored
	* beginBlock and write the difference in file bytes from the endBlock call to the
	* location of this beginBlock.  The current file position will then return to the location
	* at which endBlock was called */
//-------------------------------------------------------------------------------------------------
Int XferSave::beginBlock()
{

	// sanity
	DEBUG_ASSERTCRASH( m_fileFP != nullptr, ("Xfer begin block - file pointer for '%s' is null",
										 m_identifier.str()) );

	// get the current file position so we can back up here for the next end block call
	XferFilePos filePos;
#ifdef _WIN64
	filePos = static_cast<XferFilePos>( _ftelli64( m_fileFP ) );
#else
	filePos = ftell( m_fileFP );
#endif

	// write a placeholder
	XferBlockSize blockSize = 0;
	if( fwrite( &blockSize, sizeof( XferBlockSize ), 1, m_fileFP ) != 1 )
	{

		DEBUG_CRASH(( "XferSave::beginBlock - Error writing block size in '%s'",
									m_identifier.str() ));
		return XFER_WRITE_ERROR;

	}

	// save this block position on the top of the "stack"
	XferBlockData *top = newInstance(XferBlockData);
// impossible -- exception will be thrown (srj)
//	if( top == nullptr )
//	{
//
//		DEBUG_CRASH(( "XferSave - Begin block, out of memory - can't save block stack data" ));
//		return XFER_OUT_OF_MEMORY;
//
//	}  // end if

	top->filePos = filePos;
	top->next = m_blockStack;
	m_blockStack = top;

	return XFER_OK;

}

//-------------------------------------------------------------------------------------------------
/** Do the tail end as described in beginBlock above.  Back up to the last begin block,
	* write the file difference from current position to the last begin position, and put
	* current file position back to where it was */
//-------------------------------------------------------------------------------------------------
void XferSave::endBlock()
{

	// sanity
	DEBUG_ASSERTCRASH( m_fileFP != nullptr, ("Xfer end block - file pointer for '%s' is null",
										 m_identifier.str()) );

	// sanity, make sure we have a block started
	if( m_blockStack == nullptr )
	{

		DEBUG_CRASH(( "Xfer end block called, but no matching begin block was found" ));
		throw XFER_BEGIN_END_MISMATCH;

	}

	// save our current file position
	XferFilePos currentFilePos;
#ifdef _WIN64
	currentFilePos = static_cast<XferFilePos>( _ftelli64( m_fileFP ) );
#else
	currentFilePos = ftell( m_fileFP );
#endif

	// pop the block descriptor off the top of the block stack
	XferBlockData *top = m_blockStack;
	m_blockStack = m_blockStack->next;

	// rewind the file to the block position
#ifdef _WIN64
	_fseeki64( m_fileFP, static_cast<__int64>( top->filePos ), SEEK_SET );
#else
	fseek( m_fileFP, top->filePos, SEEK_SET );
#endif

	// write the size in bytes between the block position and what is our current file position
	XferBlockSize blockSize = currentFilePos - top->filePos - sizeof( XferBlockSize );
	if( fwrite( &blockSize, sizeof( XferBlockSize ), 1, m_fileFP ) != 1 )
	{

		DEBUG_CRASH(( "Error writing block size to file '%s'", m_identifier.str() ));
		throw XFER_WRITE_ERROR;

	}

	// place the file pointer back to the current position
#ifdef _WIN64
	_fseeki64( m_fileFP, static_cast<__int64>( currentFilePos ), SEEK_SET );
#else
	fseek( m_fileFP, currentFilePos, SEEK_SET );
#endif

	// delete the block data as it's all used up now
	deleteInstance(top);

}

//-------------------------------------------------------------------------------------------------
/** Skip forward 'dataSize' bytes in the file */
//-------------------------------------------------------------------------------------------------
void XferSave::skip( Int dataSize )
{

	// sanity
	DEBUG_ASSERTCRASH( m_fileFP != nullptr, ("XferSave - file pointer for '%s' is null",
										 m_identifier.str()) );


	// skip forward dataSize bytes
#ifdef _WIN64
	_fseeki64( m_fileFP, static_cast<__int64>( dataSize ), SEEK_CUR );
#else
	fseek( m_fileFP, dataSize, SEEK_CUR );
#endif

}

// ------------------------------------------------------------------------------------------------
/** Entry point for xfering a snapshot */
// ------------------------------------------------------------------------------------------------
void XferSave::xferSnapshot( Snapshot *snapshot )
{

	if( snapshot == nullptr )
	{

		DEBUG_CRASH(( "XferSave::xferSnapshot - Invalid parameters" ));
		throw XFER_INVALID_PARAMETERS;

	}

	// run the xfer function of the snapshot
	snapshot->xfer( this );

}

// ------------------------------------------------------------------------------------------------
/** Save ascii string */
// ------------------------------------------------------------------------------------------------
void XferSave::xferAsciiString( AsciiString *asciiStringData )
{

	// sanity
	if( asciiStringData->getLength() > 255 )
	{

		DEBUG_CRASH(( "XferSave cannot save this unicode string because it's too long.  Change the size of the length header (but be sure to preserve save file compatability" ));
		throw XFER_STRING_ERROR;

	}

	// save length of string to follow
	UnsignedByte len = asciiStringData->getLength();
	xferUnsignedByte( &len );

	// save string data
	if( len > 0 )
		xferUser( (void *)asciiStringData->str(), sizeof( Byte ) * len );

}

// ------------------------------------------------------------------------------------------------
/** Save unicodee string */
// ------------------------------------------------------------------------------------------------
void XferSave::xferUnicodeString( UnicodeString *unicodeStringData )
{

	// sanity
	if( unicodeStringData->getLength() > 255 )
	{

		DEBUG_CRASH(( "XferSave cannot save this unicode string because it's too long.  Change the size of the length header (but be sure to preserve save file compatability" ));
		throw XFER_STRING_ERROR;

	}

	// save length of string to follow
	UnsignedByte len = unicodeStringData->getLength();
	xferUnsignedByte( &len );

	// save string data
	if( len > 0 )
		xferUser( (void *)unicodeStringData->str(), sizeof( WideChar ) * len );

}

//-------------------------------------------------------------------------------------------------
/** Perform the write operation */
//-------------------------------------------------------------------------------------------------
void XferSave::xferImplementation( void *data, Int dataSize )
{

	// sanity
	DEBUG_ASSERTCRASH( m_fileFP != nullptr, ("XferSave - file pointer for '%s' is null",
										 m_identifier.str()) );

	// write data to file
	if( fwrite( data, dataSize, 1, m_fileFP ) != 1 )
	{

		DEBUG_CRASH(( "XferSave - Error writing to file '%s'", m_identifier.str() ));
		throw XFER_WRITE_ERROR;

	}

}
