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

///////////////////////////////////////////////////////////////////////////////////////
// FILE: FileTransfer.cpp
// Author: Matthew D. Campbell, December 2002
// Description: File Transfer wrapper using TheNetwork
///////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/LoadScreen.h"
#include "GameClient/MapUtil.h"
#include "GameClient/Shell.h"
#if defined(_WIN64)
#include "Common/LocalFileSystem.h"
#include "Common/file.h"
#endif
#include "GameNetwork/FileTransfer.h"
#include "GameNetwork/networkutil.h"
#include "Lib/FileTransferTimeout.h"
#if defined(_WIN64)
#include "Lib/NetworkEpochHandshake.h"
#endif

//-------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------

static Bool doFileTransfer( AsciiString filename, MapTransferLoadScreen *ls, Int mask )
{
	Bool fileTransferDone = FALSE;
	Int fileTransferPercent = 0;
	Int i;

	if (mask)
	{
#if defined(_WIN64)
		if (TheGameInfo->amIHost())
		{
			File *source = TheLocalFileSystem->openFile(filename.str(), File::READ);
			if (source == nullptr)
				return FALSE;
			source->close();
		}
#endif
		ls->setCurrentFilename(filename);
		UnsignedInt startTime = timeGetTime();
		const Int timeoutPeriod = 2*60*1000;
		ls->processTimeout(timeoutPeriod/1000);

		ls->update(0);
		fileTransferDone = FALSE;
		fileTransferPercent = 0;

		UnsignedShort fileCommandID = 0;
		Bool sentFile = FALSE;
		const Bool isHost = TheGameInfo->amIHost();
		Bool announcedFile = !isHost;
		// Preserve the legacy Win32 transfer ordering and pacing.  The x64
		// path deliberately defers this announce until the NET3 handshake has
		// completed below.
#if !defined(_WIN64)
		if (isHost)
		{
			Sleep(500);
			fileCommandID = TheNetwork->sendFileAnnounce(filename, mask);
			announcedFile = TRUE;
		}
#endif
		if (!isHost)
			sentFile = TRUE;

		DEBUG_LOG(("Starting file transfer loop"));

		while (!fileTransferDone)
		{
			// The x64 runtime exchanges NET3 before any file command is
			// allowed onto the shared transport.  Keep servicing the existing
			// load-screen/network pump while that non-blocking state machine is
			// pending; a failed or timed-out exchange aborts this transfer.
#if defined(_WIN64)
			if (TheNetwork != nullptr && !TheNetwork->isNetworkHelloReady())
			{
				if (TheNetwork->hasNetworkHelloFailure())
					return FALSE;

				const UnsignedInt now = timeGetTime();
				if (rts::file_transfer::IsTimedOut(now, startTime, timeoutPeriod))
					break;
				ls->processTimeout(static_cast<Int>(rts::file_transfer::RemainingSeconds(
					now, startTime, timeoutPeriod)));
				ls->update(0);
				continue;
			}
#endif

			if (isHost && !announcedFile && TheNetwork->areAllQueuesEmpty())
			{
				fileCommandID = TheNetwork->sendFileAnnounce(filename, mask);
				announcedFile = TRUE;
			}

			if (isHost && announcedFile && !sentFile && TheNetwork->areAllQueuesEmpty())
			{
				TheNetwork->sendFile(filename, mask, fileCommandID);
				sentFile = TRUE;
			}

			// get the progress for each player, and take the min for our overall progress
			fileTransferDone = TRUE;
			fileTransferPercent = 100;
			for (i=1; i<MAX_SLOTS; ++i)
			{
				if ((mask & (1 << i)) != 0)
				{
					Int slotTransferPercent = TheNetwork->getFileTransferProgress(i, filename);
					fileTransferPercent = min(fileTransferPercent, slotTransferPercent);

					if (slotTransferPercent == 0)
						ls->processProgress(i, slotTransferPercent, "MapTransfer:Preparing");
					else if (slotTransferPercent < 100)
						ls->processProgress(i, slotTransferPercent, "MapTransfer:Recieving");
					else
						ls->processProgress(i, slotTransferPercent, "MapTransfer:Done");
				}
			}
			if (fileTransferPercent < 100)
			{
				fileTransferDone = FALSE;
				if (fileTransferPercent == 0)
					ls->processProgress(0, fileTransferPercent, "MapTransfer:Preparing");
				else
					ls->processProgress(0, fileTransferPercent, "MapTransfer:Sending");
			}
			else
			{
				DEBUG_LOG(("File transfer is 100%%!"));
				ls->processProgress(0, fileTransferPercent, "MapTransfer:Done");
			}

			const UnsignedInt now = timeGetTime();
			if (rts::file_transfer::IsTimedOut(now, startTime, timeoutPeriod)) // bail if we don't finish in a reasonable amount of time
			{
				DEBUG_LOG(("Timing out file transfer"));
				break;
			}
			else
			{
				ls->processTimeout(static_cast<Int>(rts::file_transfer::RemainingSeconds(
					now, startTime, timeoutPeriod)));
			}

			ls->update(fileTransferPercent);
		}

		if (!fileTransferDone)
		{
			return FALSE;
		}
	}

	return TRUE;
}

//-------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------

AsciiString GetBasePathFromPath( AsciiString path )
{
	const char *s = path.reverseFind('\\');
	if (s)
	{
		Int len = s - path.str();

		AsciiString base;
		char *buf = base.getBufferForRead(len + 1);
		memcpy(buf, path.str(), len);
		buf[len] = 0;
		return buf;
	}
	return AsciiString::TheEmptyString;
}

AsciiString GetFileFromPath( AsciiString path )
{
	const char *s = path.reverseFind('\\');
	if (s)
		return s+1;
	return path;
}

AsciiString GetExtensionFromFile( AsciiString fname )
{
	const char *s = fname.reverseFind('.');
	if (s)
		return s+1;
	return fname;
}

AsciiString GetBaseFileFromFile( AsciiString fname )
{
	const char *s = fname.reverseFind('.');
	if (s)
	{
		Int len = s - fname.str();

		AsciiString base;
		char *buf = base.getBufferForRead(len + 1);
		memcpy(buf, fname.str(), len);
		buf[len] = 0;
		return buf;
	}
	return AsciiString::TheEmptyString;
}

AsciiString GetPreviewFromMap( AsciiString path )
{
	AsciiString fname = GetBaseFileFromFile(GetFileFromPath(path));
	AsciiString base = GetBasePathFromPath(path);

	AsciiString out;
	out.format("%s\\%s.tga", base.str(), fname.str());
	return out;
}

AsciiString GetINIFromMap( AsciiString path )
{
	AsciiString base = GetBasePathFromPath(path);

	AsciiString out;
	out.format("%s\\map.ini", base.str());
	return out;
}

AsciiString GetStrFileFromMap( AsciiString path )
{
	AsciiString base = GetBasePathFromPath(path);

	AsciiString out;
	out.format("%s\\map.str", base.str());
	return out;
}

AsciiString GetSoloINIFromMap( AsciiString path )
{
	AsciiString base = GetBasePathFromPath(path);

	AsciiString out;
	out.format("%s\\solo.ini", base.str());
	return out;
}

AsciiString GetAssetUsageFromMap( AsciiString path )
{
	AsciiString base = GetBasePathFromPath(path);

	AsciiString out;
	out.format("%s\\assetusage.txt", base.str());
	return out;
}

AsciiString GetReadmeFromMap( AsciiString path )
{
	AsciiString base = GetBasePathFromPath(path);

	AsciiString out;
	out.format("%s\\readme.txt", base.str());
	return out;
}

//-------------------------------------------------------------------------------------
//-------------------------------------------------------------------------------------

Bool DoAnyMapTransfers(GameInfo *game, Bool allowSidecarTransfer)
{
	TheGameInfo = game;
	Int mask = 0;
	Int i=0;
#if defined(_WIN64)
	// The hello carries each peer's actual sidecar identity. Wait for it
	// before selecting transfer recipients, including Quick Match peers.
	const UnsignedInt helloStart = timeGetTime();
	while (!TheNetwork->isNetworkHelloReady())
	{
		if (TheNetwork->hasNetworkHelloFailure() ||
			rts::file_transfer::IsTimedOut(timeGetTime(), helloStart, 11000U))
			return FALSE;
		TheNetwork->liteupdate();
		Sleep(1);
	}
	UnsignedInt hostContentsMask = 0U;
	UnsignedInt hostSidecarCRC = 0U;
	if (!TheNetwork->getNetworkMapSidecarIdentity(0, &hostContentsMask,
		&hostSidecarCRC))
		return FALSE;
	for (i = 1; i < MAX_SLOTS; ++i)
	{
		if (!game->getConstSlot(i)->isHuman())
			continue;
		UnsignedInt peerContentsMask = 0U;
		UnsignedInt peerSidecarCRC = 0U;
		if (!TheNetwork->getNetworkMapSidecarIdentity(i, &peerContentsMask,
			&peerSidecarCRC))
			return FALSE;
		const rts::network_epoch::NetworkSidecarTransferDecision decision =
			rts::network_epoch::DecideNetworkSidecarTransfer(
				hostContentsMask, hostSidecarCRC,
				peerContentsMask, peerSidecarCRC, allowSidecarTransfer);
		// A host cannot transfer an absent sidecar. Quick Match has no
		// authoritative file-transfer sender. Both cases fail before writing.
		if (decision == rts::network_epoch::NetworkSidecarTransferDecision::Reject)
			return FALSE;
		if (decision == rts::network_epoch::NetworkSidecarTransferDecision::Transfer)
			mask = static_cast<Int>(rts::network_epoch::AddNetworkMapTransferRecipient(
				static_cast<UnsignedInt>(mask), i, false, true));
	}
#endif
	for (i=1; i<MAX_SLOTS; ++i)
	{
		if (TheGameInfo->getConstSlot(i)->isHuman() && !TheGameInfo->getConstSlot(i)->hasMap())
		{
#if defined(_WIN64)
			if (!allowSidecarTransfer)
				return FALSE; // Quick Match has no authoritative transfer sender.
#endif
			DEBUG_LOG(("Adding player %d to transfer mask", i));
#if defined(_WIN64)
			mask = static_cast<Int>(rts::network_epoch::AddNetworkMapTransferRecipient(
				static_cast<UnsignedInt>(mask), i, true, false));
#else
			mask |= (1<<i);
#endif
		}
	}
	if (!mask)
	{
#if defined(_WIN64)
		UnsignedInt localSidecarCRC = 0U;
		if (!GetMapSimulationSidecarCRC(game->getMap(), &localSidecarCRC))
			return FALSE;
		return rts::network_epoch::IsNetworkMapPackageReady(TRUE,
			game->getMapCRC(), GetMapFileCRC(game->getMap()),
			hostContentsMask, hostSidecarCRC,
			GetMapSimulationSidecarMask(game->getMap()),
			localSidecarCRC);
#else
		return TRUE;
#endif
	}

	TheShell->hideShell();
	MapTransferLoadScreen *ls = NEW MapTransferLoadScreen;
	ls->init(TheGameInfo);
	Bool ok = TRUE;
	Int contentsMask = TheGameInfo->getMapContentsMask();
#if defined(_WIN64)
	contentsMask = static_cast<Int>(hostContentsMask);
#endif
	if (contentsMask & 2)
		ok = doFileTransfer(GetPreviewFromMap(game->getMap()), ls, mask);
	if (ok && contentsMask & 4)
		ok = doFileTransfer(GetINIFromMap(game->getMap()), ls, mask);
	if (ok && contentsMask & 8)
		ok = doFileTransfer(GetStrFileFromMap(game->getMap()), ls, mask);
	if (ok && contentsMask & 16)
		ok = doFileTransfer(GetSoloINIFromMap(game->getMap()), ls, mask);
	if (ok && contentsMask & 32)
		ok = doFileTransfer(GetAssetUsageFromMap(game->getMap()), ls, mask);
	if (ok && contentsMask & 64)
		ok = doFileTransfer(GetReadmeFromMap(game->getMap()), ls, mask);
	if (ok)
		ok = doFileTransfer(game->getMap(), ls, mask);
	delete ls;
	ls = nullptr;
#if defined(_WIN64)
	if (ok)
	{
		UnsignedInt localSidecarCRC = 0U;
		ok = GetMapSimulationSidecarCRC(game->getMap(), &localSidecarCRC) &&
			rts::network_epoch::IsNetworkMapPackageReady(TRUE,
				game->getMapCRC(), GetMapFileCRC(game->getMap()),
				hostContentsMask, hostSidecarCRC,
				GetMapSimulationSidecarMask(game->getMap()),
				localSidecarCRC);
	}
#endif
	if (!ok)
		TheShell->showShell();
	return ok;
}
