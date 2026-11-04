// SPDX-License-Identifier: MIT
/** @file
   Copyright (c) 2026 Jens Fresenborg

   Types and constants under the names of the TcAdsDll header TcAdsDef.h, so
   code written against the TcAdsDll compiles with libads-client. Written
   from the documented interface, not copied from Beckhoff's header. C++ only.
 */

#pragma once

#include "AdsDef.h"

#include <cstddef>
#include <cstring>

#ifndef ANYSIZE_ARRAY
#define ANYSIZE_ARRAY 1
#endif

#ifndef TCADSDLL_EXPORT
#define TCADSDLL_EXPORT
#endif

/** Length of the device name returned by AdsSyncReadDeviceInfoReq() */
#define ADS_FIXEDNAMESIZE 16

#define ADSDATATYPEFLAG_DATATYPE 0x00000001
#define ADSDATATYPEFLAG_DATAITEM 0x00000002
#define ADSDATATYPE_VERSION_NEWEST 0x00000001

/** Start of the sample behind a notification header */
#define ADSNOTIFICATION_PDATA(pHeader) \
	(reinterpret_cast<ads_ui8 *>((pHeader)->data))

#pragma pack(push, 1)

/** Answer of ADSIGRP_SYM_UPLOADINFO */
struct AdsSymbolUploadInfo {
	ads_ui32 nSymbols;
	ads_ui32 nSymSize;
};
typedef AdsSymbolUploadInfo *PAdsSymbolUploadInfo;

/** Bounds of one array dimension in an AdsDatatypeEntry */
struct AdsDatatypeArrayInfo {
	ads_ui32 lBound;
	ads_ui32 elements;
};
typedef AdsDatatypeArrayInfo *PAdsDatatypeArrayInfo;

/**
 * Header of a data type description (ADSIGRP_SYM_DT_UPLOAD). It is followed
 * by the zero terminated name, type and comment, then arrayDim
 * AdsDatatypeArrayInfo and subItems entries of the same kind.
 */
struct AdsDatatypeEntry {
	ads_ui32 entryLength; // length of the complete entry
	ads_ui32 version; // version of this structure
	ads_ui32 hashValue; // hash of the data type
	ads_ui32 typeHashValue; // hash of the base type
	ads_ui32 size; // size in bytes
	ads_ui32 offs; // offset within the parent data type in bytes
	ads_ui32 dataType; // ADS data type, if this is an alias
	ads_ui32 flags; // ADSDATATYPEFLAG_*
	ads_ui16 nameLength; // without the terminating zero
	ads_ui16 typeLength; // without the terminating zero
	ads_ui16 commentLength; // without the terminating zero
	ads_ui16 arrayDim;
	ads_ui16 subItems;
};
typedef AdsDatatypeEntry *PAdsDatatypeEntry, **PPAdsDatatypeEntry;

#pragma pack(pop)

/** @name Access to the strings behind an AdsSymbolEntry */
///@{
inline char *PADSSYMBOLNAME(const void *p)
{
	return reinterpret_cast<char *>(
		const_cast<AdsSymbolEntry *>(
			static_cast<const AdsSymbolEntry *>(p)) +
		1);
}

inline char *PADSSYMBOLTYPE(const void *p)
{
	return PADSSYMBOLNAME(p) +
	       static_cast<const AdsSymbolEntry *>(p)->nameLength + 1;
}

inline char *PADSSYMBOLCOMMENT(const void *p)
{
	return PADSSYMBOLTYPE(p) +
	       static_cast<const AdsSymbolEntry *>(p)->typeLength + 1;
}

/** Next entry of a symbol upload, nullptr after the last one */
inline PAdsSymbolEntry PADSNEXTSYMBOLENTRY(const void *p)
{
	auto next = const_cast<char *>(static_cast<const char *>(p)) +
		    static_cast<const AdsSymbolEntry *>(p)->entryLength;
	ads_ui32 length;
	memcpy(&length, next, sizeof(length));
	return length ? reinterpret_cast<PAdsSymbolEntry>(next) : nullptr;
}
///@}

/** @name Access to the parts behind an AdsDatatypeEntry */
///@{
inline char *PADSDATATYPENAME(const void *p)
{
	return reinterpret_cast<char *>(
		const_cast<AdsDatatypeEntry *>(
			static_cast<const AdsDatatypeEntry *>(p)) +
		1);
}

inline char *PADSDATATYPETYPE(const void *p)
{
	return PADSDATATYPENAME(p) +
	       static_cast<const AdsDatatypeEntry *>(p)->nameLength + 1;
}

inline char *PADSDATATYPECOMMENT(const void *p)
{
	return PADSDATATYPETYPE(p) +
	       static_cast<const AdsDatatypeEntry *>(p)->typeLength + 1;
}

inline PAdsDatatypeArrayInfo PADSDATATYPEARRAYINFO(const void *p)
{
	return reinterpret_cast<PAdsDatatypeArrayInfo>(
		PADSDATATYPECOMMENT(p) +
		static_cast<const AdsDatatypeEntry *>(p)->commentLength + 1);
}

/** Sub item number iItem of a structured data type, nullptr if there is none */
inline PAdsDatatypeEntry AdsDatatypeStructItem(PAdsDatatypeEntry p,
					       ads_ui16 iItem)
{
	if (iItem >= p->subItems) {
		return nullptr;
	}
	auto item = reinterpret_cast<char *>(PADSDATATYPEARRAYINFO(p) +
					     p->arrayDim);
	for (ads_ui16 i = 0; i < iItem; ++i) {
		item += reinterpret_cast<PAdsDatatypeEntry>(item)->entryLength;
	}
	return reinterpret_cast<PAdsDatatypeEntry>(item);
}
///@}
