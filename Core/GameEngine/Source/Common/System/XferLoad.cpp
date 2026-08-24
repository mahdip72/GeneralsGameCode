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

// FILE: XferLoad.cpp /////////////////////////////////////////////////////////////////////////////
// Author: Colin Day, February 2002
// Desc:   Xfer implementation for loading from disk
///////////////////////////////////////////////////////////////////////////////////////////////////

// USER INCLUDES //////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine
#include "Common/Debug.h"
#include "Common/GameState.h"
#include "Common/Snapshot.h"
#include "Common/XferLoad.h"

#ifdef _WIN64
#include "Lib/RuntimeEpochContract.h"

#include <array>
#include <limits>

namespace
{

void ValidateNativeRuntimeEpochSave(FILE *file,
	std::uint32_t executableCrc,
	std::uint32_t iniCrc)
{
	std::array<rts::runtime_epoch::Byte, rts::runtime_epoch::kHeaderSize> encoded = {{}};
	if( fread( encoded.data(), 1, encoded.size(), file ) != encoded.size() )
	{
		DEBUG_CRASH(( "XferLoad - Native x64 save header is truncated" ));
		throw XFER_READ_ERROR;
	}

	rts::runtime_epoch::SaveHeader header;
	if( !rts::runtime_epoch::Decode( encoded.data(), encoded.size(), &header ) )
	{
		DEBUG_CRASH(( "XferLoad - Native x64 save header is invalid" ));
		throw XFER_INVALID_PARAMETERS;
	}

	const __int64 fileEnd = _fseeki64( file, 0, SEEK_END ) == 0 ? _ftelli64( file ) : -1;
	if( fileEnd < static_cast<__int64>(rts::runtime_epoch::kHeaderSize) )
	{
		DEBUG_CRASH(( "XferLoad - Native x64 save file is truncated" ));
		throw XFER_READ_ERROR;
	}

	const std::uint64_t payloadFileSize = static_cast<std::uint64_t>(
		fileEnd - static_cast<__int64>(rts::runtime_epoch::kHeaderSize));
	if( payloadFileSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) )
	{
		DEBUG_CRASH(( "XferLoad - Native x64 save payload is too large" ));
		throw XFER_INVALID_PARAMETERS;
	}

	rts::runtime_epoch::ValidationOptions options;
	options.expectedBuildCompatibilityId =
		rts::runtime_epoch::BuildCompatibilityIdFromExecutableCrc(executableCrc);
	options.expectedContentHash =
		rts::runtime_epoch::ContentHashFromIniCrc(iniCrc);
	options.maxPayloadByteCount = payloadFileSize;
	options.requireBuildCompatibilityMatch = true;
	options.requireContentHashMatch = true;
	const rts::runtime_epoch::ValidationResult headerResult =
		rts::runtime_epoch::Validate( header, options );
	if( !headerResult.ok() || header.payloadByteCount != payloadFileSize )
	{
		DEBUG_CRASH(( "XferLoad - Native x64 save header does not match the file" ));
		throw XFER_INVALID_PARAMETERS;
	}

	if( _fseeki64( file, static_cast<__int64>(rts::runtime_epoch::kHeaderSize), SEEK_SET ) != 0 )
	{
		DEBUG_CRASH(( "XferLoad - Native x64 save payload cannot be positioned" ));
		throw XFER_READ_ERROR;
	}

	rts::runtime_epoch::PayloadChecksumAccumulator checksum;
	std::array<rts::runtime_epoch::Byte, 64U * 1024U> buffer = {{}};
	std::uint64_t remaining = payloadFileSize;
	while( remaining != 0U )
	{
		const std::size_t requested = static_cast<std::size_t>(
			remaining < buffer.size() ? remaining : buffer.size());
		const std::size_t read = fread( buffer.data(), 1, requested, file );
		if( read != requested )
		{
			DEBUG_CRASH(( "XferLoad - Native x64 save payload is truncated" ));
			throw XFER_READ_ERROR;
		}
		checksum.update( buffer.data(), read );
		remaining -= static_cast<std::uint64_t>(read);
	}

	if( checksum.byteCount() != header.payloadByteCount ||
		checksum.finish() != header.payloadChecksum )
	{
		DEBUG_CRASH(( "XferLoad - Native x64 save payload checksum mismatch" ));
		throw XFER_INVALID_PARAMETERS;
	}

	if( _fseeki64( file, static_cast<__int64>(rts::runtime_epoch::kHeaderSize), SEEK_SET ) != 0 )
	{
		DEBUG_CRASH(( "XferLoad - Native x64 save payload cannot be rewound" ));
		throw XFER_READ_ERROR;
	}
}

} // namespace
#endif

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
XferLoad::XferLoad()
{

	m_xferMode = XFER_LOAD;
	m_fileFP = nullptr;

#ifdef _WIN64
	m_runtimeEpochExecutableCrc = 0U;
	m_runtimeEpochIniCrc = 0U;
	m_runtimeEpochIdentityConfigured = FALSE;
#endif

}

//-------------------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------------------
XferLoad::~XferLoad()
{

	// warn the user if a file was left open
	if( m_fileFP != nullptr )
	{

		DEBUG_CRASH(( "Warning: Xfer file '%s' was left open", m_identifier.str() ));
		close();

	}

}

//-------------------------------------------------------------------------------------------------
#ifdef _WIN64
void XferLoad::setRuntimeEpochIdentity( std::uint32_t executableCrc, std::uint32_t iniCrc )
{
	if( m_fileFP != nullptr )
	{
		DEBUG_CRASH(( "XferLoad::setRuntimeEpochIdentity - file is already open" ));
		throw XFER_FILE_ALREADY_OPEN;
	}

	m_runtimeEpochExecutableCrc = executableCrc;
	m_runtimeEpochIniCrc = iniCrc;
	m_runtimeEpochIdentityConfigured = TRUE;
}
#endif

/** Open file 'identifier' for reading */
//-------------------------------------------------------------------------------------------------
void XferLoad::open( AsciiString identifier )
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
	m_fileFP = fopen( identifier.str(), "rb" );
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
		DEBUG_CRASH(( "XferLoad::open - Native x64 identity was not configured" ));
		throw XFER_INVALID_PARAMETERS;
	}

	try
	{
		ValidateNativeRuntimeEpochSave(
			m_fileFP, m_runtimeEpochExecutableCrc, m_runtimeEpochIniCrc);
	}
	catch( ... )
	{
		fclose( m_fileFP );
		m_fileFP = nullptr;
		m_identifier.clear();
		throw;
	}
#endif

}

//-------------------------------------------------------------------------------------------------
/** Close our current file */
//-------------------------------------------------------------------------------------------------
void XferLoad::close()
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
/** Read a block size descriptor from the file at the current position */
//-------------------------------------------------------------------------------------------------
Int XferLoad::beginBlock()
{

	// sanity
	DEBUG_ASSERTCRASH( m_fileFP != nullptr, ("Xfer begin block - file pointer for '%s' is null",
										 m_identifier.str()) );

	// read block size
	XferBlockSize blockSize;
	if( fread( &blockSize, sizeof( XferBlockSize ), 1, m_fileFP ) != 1 )
	{

		DEBUG_CRASH(( "Xfer - Error reading block size for '%s'", m_identifier.str() ));
		return 0;

	}

	// return the block size
	return blockSize;

}

// ------------------------------------------------------------------------------------------------
/** End block ... this does nothing when reading */
// ------------------------------------------------------------------------------------------------
void XferLoad::endBlock()
{

}

//-------------------------------------------------------------------------------------------------
/** Skip forward 'dataSize' bytes in the file */
//-------------------------------------------------------------------------------------------------
void XferLoad::skip( Int dataSize )
{

	// sanity
	DEBUG_ASSERTCRASH( m_fileFP != nullptr, ("XferLoad::skip - file pointer for '%s' is null",
										 m_identifier.str()) );

	// sanity
	DEBUG_ASSERTCRASH( dataSize >=0, ("XferLoad::skip - dataSize '%d' must be greater than 0",
										 dataSize) );

	// skip datasize in the file from the current position
	int seekResult;
#ifdef _WIN64
	seekResult = _fseeki64( m_fileFP, static_cast<__int64>( dataSize ), SEEK_CUR );
#else
	seekResult = fseek( m_fileFP, dataSize, SEEK_CUR );
#endif
	if( seekResult != 0 )
		throw XFER_SKIP_ERROR;

}

// ------------------------------------------------------------------------------------------------
/** Entry point for xfering a snapshot */
// ------------------------------------------------------------------------------------------------
void XferLoad::xferSnapshot( Snapshot *snapshot )
{

	if( snapshot == nullptr )
	{

		DEBUG_CRASH(( "XferLoad::xferSnapshot - Invalid parameters" ));
		throw XFER_INVALID_PARAMETERS;

	}

	// run the xfer function of the snapshot
	snapshot->xfer( this );

	// add this snapshot to the game state for later post processing if not restricted
	if( BitIsSet( getOptions(), XO_NO_POST_PROCESSING ) == FALSE )
		TheGameState->addPostProcessSnapshot( snapshot );

}

// ------------------------------------------------------------------------------------------------
/** Read string from file and store in ascii string */
// ------------------------------------------------------------------------------------------------
void XferLoad::xferAsciiString( AsciiString *asciiStringData )
{

	// read bytes of string length to follow
	UnsignedByte len;
	xferUnsignedByte( &len );

	// read all the string data
	const Int MAX_XFER_LOAD_STRING_BUFFER = 1024;
	static Char buffer[ MAX_XFER_LOAD_STRING_BUFFER ];

	if( len > 0 )
		xferUser( buffer, sizeof( Byte ) * len );
	buffer[ len ] = 0;  // terminate

	// save into ascii string
	asciiStringData->set( buffer );

}

// ------------------------------------------------------------------------------------------------
/** Read string from file and store in unicode string */
// ------------------------------------------------------------------------------------------------
void XferLoad::xferUnicodeString( UnicodeString *unicodeStringData )
{

	// read bytes of string length to follow
	UnsignedByte len;
	xferUnsignedByte( &len );

	// read all the string data
	const Int MAX_XFER_LOAD_STRING_BUFFER = 1024;
	static WideChar buffer[ MAX_XFER_LOAD_STRING_BUFFER ];

	if( len > 0 )
		xferUser( buffer, sizeof( WideChar ) * len );
	buffer[ len ] = 0;  // terminate

	// save into unicode string
	unicodeStringData->set( buffer );

}

//-------------------------------------------------------------------------------------------------
/** Perform the read operation */
//-------------------------------------------------------------------------------------------------
void XferLoad::xferImplementation( void *data, Int dataSize )
{

	// sanity
	DEBUG_ASSERTCRASH( m_fileFP != nullptr, ("XferLoad - file pointer for '%s' is null",
										 m_identifier.str()) );

	// read data from file
	if( fread( data, dataSize, 1, m_fileFP ) != 1 )
	{

		DEBUG_CRASH(( "XferLoad - Error reading from file '%s'", m_identifier.str() ));
		throw XFER_READ_ERROR;

	}

}

