// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2026 Jens Fresenborg
 */

#include "AdsSymbols.h"
#include "AdsDef.h"
#include "AdsDevice.h"
#include "AdsException.h"

#include <algorithm>
#include <cstdint>

namespace
{
// Flags of a symbol entry
const uint32_t SYMBOL_PERSISTENT = 0x0001;
const uint32_t SYMBOL_BITVALUE = 0x0002;
const uint32_t SYMBOL_REFERENCETO = 0x0004;
const uint32_t SYMBOL_TYPEGUID = 0x0008;
const uint32_t SYMBOL_ATTRIBUTES = 0x1000;
const uint32_t SYMBOL_EXTENDEDFLAGS = 0x8000;

// Flags of a data type entry
const uint32_t TYPE_BITVALUES = 0x0020;
const uint32_t TYPE_PROPITEM = 0x0040;
const uint32_t TYPE_TYPEGUID = 0x0080;
const uint32_t TYPE_COPYMASK = 0x0200;
const uint32_t TYPE_METHODINFOS = 0x0800;
const uint32_t TYPE_ATTRIBUTES = 0x1000;
const uint32_t TYPE_ENUMINFOS = 0x2000;

const size_t SYMBOL_HEADER = 30; // entryLength … commentLength
const size_t TYPE_HEADER = 42;
const size_t GUID_SIZE = 16;

// Code page 1252, 0x80 to 0x9F; the rest matches Latin-1. 0 = not defined.
const uint16_t cp1252[32] = {
	0x20AC, 0,	0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,	0x017D, 0,
	0,	0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,	0x017E, 0x0178
};

void AppendUtf8(std::string &out, uint32_t c)
{
	if (c < 0x80) {
		out += static_cast<char>(c);
	} else if (c < 0x800) {
		out += static_cast<char>(0xC0 | (c >> 6));
		out += static_cast<char>(0x80 | (c & 0x3F));
	} else {
		out += static_cast<char>(0xE0 | (c >> 12));
		out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
		out += static_cast<char>(0x80 | (c & 0x3F));
	}
}

std::string ToUtf8(const char *text, size_t length, uint32_t codePage)
{
	if (codePage != 1252) {
		return std::string(text, length);
	}
	std::string out;
	out.reserve(length);
	for (size_t i = 0; i < length; ++i) {
		const auto c = static_cast<uint8_t>(text[i]);
		if (c >= 0x80 && c < 0xA0) {
			const auto u = cp1252[c - 0x80];
			AppendUtf8(out, u ? u : 0xFFFD);
		} else {
			AppendUtf8(out, c);
		}
	}
	return out;
}

/** Reads little endian values from [pos, end); throws on any access beyond end */
struct Reader {
	const uint8_t *pos;
	const uint8_t *end;
	uint32_t codePage;

	size_t Left() const
	{
		return static_cast<size_t>(end - pos);
	}

	void Need(size_t n) const
	{
		if (n > Left()) {
			throw AdsException(ADSERR_DEVICE_INVALIDDATA);
		}
	}

	void Skip(size_t n)
	{
		Need(n);
		pos += n;
	}

	uint64_t Unsigned(size_t n)
	{
		Need(n);
		uint64_t v = 0;
		for (size_t i = 0; i < n; ++i) {
			v |= static_cast<uint64_t>(pos[i]) << (8 * i);
		}
		pos += n;
		return v;
	}

	uint8_t U8()
	{
		return static_cast<uint8_t>(Unsigned(1));
	}
	uint16_t U16()
	{
		return static_cast<uint16_t>(Unsigned(2));
	}
	uint32_t U32()
	{
		return static_cast<uint32_t>(Unsigned(4));
	}

	/** Text of the given length, followed by a terminating zero */
	std::string Text(size_t length)
	{
		Need(length + 1);
		if (pos[length] != 0) {
			throw AdsException(ADSERR_DEVICE_INVALIDDATA);
		}
		auto s = ToUtf8(reinterpret_cast<const char *>(pos), length,
				codePage);
		pos += length + 1;
		return s;
	}

	/** Reader for the next entry, which starts with its own length */
	Reader Entry(size_t minimum)
	{
		Need(4);
		const auto length = static_cast<size_t>(pos[0]) |
				    static_cast<size_t>(pos[1]) << 8 |
				    static_cast<size_t>(pos[2]) << 16 |
				    static_cast<size_t>(pos[3]) << 24;
		if (length < minimum) {
			throw AdsException(ADSERR_DEVICE_INVALIDDATA);
		}
		Need(length);
		Reader entry{ pos, pos + length, codePage };
		pos += length;
		return entry;
	}

	/** Behind the last entry only zeros (or nothing) may follow */
	bool AtEnd() const
	{
		for (auto p = pos; p != end; ++p) {
			if (*p) {
				return false;
			}
		}
		return true;
	}
};

std::vector<AdsAttribute> ReadAttributes(Reader &r)
{
	std::vector<AdsAttribute> attributes(r.U16());
	for (auto &a : attributes) {
		const auto nameLength = r.U8();
		const auto valueLength = r.U8();
		a.name = r.Text(nameLength);
		a.value = r.Text(valueLength);
	}
	return attributes;
}

bool IsUnsigned(uint32_t dataType)
{
	return dataType == ADST_UINT8 || dataType == ADST_UINT16 ||
	       dataType == ADST_UINT32 || dataType == ADST_UINT64 ||
	       dataType == ADST_BIT;
}

struct TypeHeader {
	uint32_t size;
	uint32_t offset;
	uint32_t dataType;
	uint32_t flags;
	uint16_t arrayDim;
	uint16_t subItems;
	std::string name;
	std::string typeName;
	std::string comment;
};

TypeHeader ReadTypeHeader(Reader &r)
{
	TypeHeader h;
	r.Skip(4 + 4 + 4 + 4); // entryLength, version, hashValue, typeHashValue
	h.size = r.U32();
	h.offset = r.U32();
	h.dataType = r.U32();
	h.flags = r.U32();
	const auto nameLength = r.U16();
	const auto typeLength = r.U16();
	const auto commentLength = r.U16();
	h.arrayDim = r.U16();
	h.subItems = r.U16();
	h.name = r.Text(nameLength);
	h.typeName = r.Text(typeLength);
	h.comment = r.Text(commentLength);
	return h;
}

std::vector<AdsArrayDim> ReadDims(Reader &r, uint16_t count)
{
	std::vector<AdsArrayDim> dims(count);
	for (auto &d : dims) {
		d.lowerBound = static_cast<int32_t>(r.U32());
		d.elements = r.U32();
	}
	return dims;
}

AdsDataMember ReadMember(Reader r)
{
	const auto h = ReadTypeHeader(r);
	ReadDims(r, h.arrayDim);
	for (uint16_t i = 0; i < h.subItems; ++i) {
		r.Entry(TYPE_HEADER); // members of a member are described by its type
	}
	AdsDataMember m;
	m.name = h.name;
	m.typeName = h.typeName;
	m.comment = h.comment;
	m.offset = h.offset;
	m.size = h.size;
	m.dataType = h.dataType;
	m.flags = h.flags;
	m.bitField = (h.flags & TYPE_BITVALUES) != 0;
	m.property = (h.flags & TYPE_PROPITEM) != 0;
	if (h.flags & TYPE_TYPEGUID) {
		r.Skip(GUID_SIZE);
	}
	if (h.flags & TYPE_ATTRIBUTES) {
		m.attributes = ReadAttributes(r);
	}
	return m;
}

AdsDataType ReadType(Reader r)
{
	const auto h = ReadTypeHeader(r);
	AdsDataType t;
	t.name = h.name;
	t.baseType = h.typeName;
	t.comment = h.comment;
	t.size = h.size;
	t.dataType = h.dataType;
	t.flags = h.flags;
	t.dims = ReadDims(r, h.arrayDim);
	t.members.reserve(h.subItems);
	for (uint16_t i = 0; i < h.subItems; ++i) {
		t.members.push_back(ReadMember(r.Entry(TYPE_HEADER)));
	}
	if (h.flags & TYPE_TYPEGUID) {
		r.Skip(GUID_SIZE);
	}
	if (h.flags & TYPE_COPYMASK) {
		r.Skip(h.size);
	}
	if (h.flags & TYPE_METHODINFOS) {
		const auto methods = r.U16();
		for (uint16_t i = 0; i < methods; ++i) {
			r.Entry(4);
		}
	}
	if (h.flags & TYPE_ATTRIBUTES) {
		t.attributes = ReadAttributes(r);
	}
	if (h.flags & TYPE_ENUMINFOS) {
		if (h.size == 0 || h.size > 8) {
			throw AdsException(ADSERR_DEVICE_INVALIDDATA);
		}
		t.enumValues.resize(r.U16());
		for (auto &e : t.enumValues) {
			e.name = r.Text(r.U8());
			auto v = r.Unsigned(h.size);
			if (!IsUnsigned(h.dataType) && h.size < 8 &&
			    (v >> (8 * h.size - 1)) & 1) {
				v |= ~uint64_t{ 0 } << (8 * h.size); // sign
			}
			e.value = static_cast<int64_t>(v);
		}
	}
	// Whatever follows is not known here and skipped with the entry.
	return t;
}

AdsSymbol ReadSymbol(Reader r)
{
	AdsSymbol s;
	r.Skip(4); // entryLength
	s.indexGroup = r.U32();
	s.indexOffset = r.U32();
	s.size = r.U32();
	s.dataType = r.U32();
	s.flags = r.U32();
	const auto nameLength = r.U16();
	const auto typeLength = r.U16();
	const auto commentLength = r.U16();
	s.name = r.Text(nameLength);
	s.typeName = r.Text(typeLength);
	s.comment = r.Text(commentLength);
	s.bitAddress = (s.flags & SYMBOL_BITVALUE) != 0;
	s.persistent = (s.flags & SYMBOL_PERSISTENT) != 0;
	s.reference = (s.flags & SYMBOL_REFERENCETO) != 0;
	if (s.flags & SYMBOL_TYPEGUID) {
		r.Skip(GUID_SIZE);
	}
	if (s.flags & SYMBOL_ATTRIBUTES) {
		s.attributes = ReadAttributes(r);
	}
	if (s.flags & SYMBOL_EXTENDEDFLAGS) {
		r.Skip(4);
	}
	return s;
}
} // namespace

std::vector<AdsSymbol> ParseSymbols(const uint8_t *data, size_t length,
				    uint32_t codePage)
{
	Reader r{ data, data + length, codePage };
	std::vector<AdsSymbol> symbols;
	while (!r.AtEnd()) {
		symbols.push_back(ReadSymbol(r.Entry(SYMBOL_HEADER)));
	}
	return symbols;
}

std::vector<AdsDataType> ParseDataTypes(const uint8_t *data, size_t length,
					uint32_t codePage)
{
	Reader r{ data, data + length, codePage };
	std::vector<AdsDataType> types;
	while (!r.AtEnd()) {
		types.push_back(ReadType(r.Entry(TYPE_HEADER)));
	}
	return types;
}

namespace
{
const uint32_t NO_OFFSET = 0xFFFFFFFF; // members of a GVL or program, static members
const size_t MAX_ALIAS_DEPTH = 16;

std::string Lower(const std::string &s)
{
	std::string out(s);
	for (auto &c : out) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return out;
}

bool StartsWith(const std::string &s, const std::string &lowerPrefix)
{
	return Lower(s).compare(0, lowerPrefix.size(), lowerPrefix) == 0;
}

bool HasAttribute(const AdsDataType *t, const std::string &lowerName)
{
	if (!t) {
		return false;
	}
	for (const auto &a : t->attributes) {
		if (Lower(a.name) == lowerName) {
			return true;
		}
	}
	return false;
}

uint32_t Le32(const uint8_t *p)
{
	return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
	       static_cast<uint32_t>(p[2]) << 16 |
	       static_cast<uint32_t>(p[3]) << 24;
}

std::vector<uint8_t> Upload(const AdsDevice &device, uint32_t group,
			    uint32_t size)
{
	std::vector<uint8_t> data(size);
	if (!size) {
		return data;
	}
	uint32_t bytesRead = 0;
	const auto error =
		device.ReadReqEx2(group, 0, size, data.data(), &bytesRead);
	if (error) {
		throw AdsException(error);
	}
	data.resize(bytesRead);
	return data;
}

using NameIndex = std::vector<std::pair<std::string, size_t> >;

const std::pair<std::string, size_t> *Find(const NameIndex &index,
					    const std::string &name)
{
	const auto key = Lower(name);
	const auto it = std::lower_bound(
		index.begin(), index.end(), key,
		[](const std::pair<std::string, size_t> &e,
		   const std::string &k) { return e.first < k; });
	return it != index.end() && it->first == key ? &*it : nullptr;
}
} // namespace

namespace
{
const char ONLINE_CHANGES_PATH[] =
	"TwinCAT_SystemInfoVarList._AppInfo.OnlineChangeCnt";

/** OnlineChangeCnt read by name; false if the PLC has no such value */
bool ReadOnlineChangesByName(const AdsDevice &device, uint32_t &count)
{
	uint8_t value[4] = {};
	uint32_t bytesRead = 0;
	const auto error = device.ReadWriteReqEx2(
		ADSIGRP_SYM_VALBYNAME, 0, sizeof(value), value,
		sizeof(ONLINE_CHANGES_PATH), ONLINE_CHANGES_PATH, &bytesRead);
	if (error == ADSERR_DEVICE_SYMBOLNOTFOUND) {
		return false;
	}
	if (error) {
		throw AdsException(error);
	}
	if (bytesRead != sizeof(value)) {
		return false;
	}
	count = Le32(value);
	return true;
}
} // namespace

AdsSymbolTable::AdsSymbolTable(const AdsDevice &device)
	: m_Version(0)
	, m_HasOnlineChanges(false)
	, m_OnlineChangesGroup(0)
	, m_OnlineChangesOffset(0)
	, m_OnlineChanges(0)
{
	// The number of online changes is read before and after the upload:
	// if it moved, an online change came in between and the upload may
	// mix both programs, so it is done again.
	for (int attempt = 0; attempt < 3; ++attempt) {
		uint32_t before = 0;
		const bool hasCount = ReadOnlineChangesByName(device, before);
		Load(device);
		AdsElement e;
		if (!hasCount || !FindElement(ONLINE_CHANGES_PATH, e) ||
		    e.kind != AdsElement::Kind::Value || e.size != 4) {
			return;
		}
		uint8_t value[4] = {};
		uint32_t bytesRead = 0;
		const auto error = device.ReadReqEx2(e.indexGroup, e.indexOffset,
						     sizeof(value), value,
						     &bytesRead);
		if (error) {
			throw AdsException(error);
		}
		if (bytesRead != sizeof(value)) {
			throw AdsException(ADSERR_DEVICE_INVALIDDATA);
		}
		if (Le32(value) == before) {
			m_HasOnlineChanges = true;
			m_OnlineChangesGroup = e.indexGroup;
			m_OnlineChangesOffset = e.indexOffset;
			m_OnlineChanges = before;
			return;
		}
	}
	throw AdsException(ADSERR_DEVICE_SYMBOLVERSIONINVALID);
}

void AdsSymbolTable::Load(const AdsDevice &device)
{
	uint8_t info[64] = {};
	uint32_t bytesRead = 0;
	const auto error = device.ReadReqEx2(ADSIGRP_SYM_UPLOADINFO2, 0,
					     sizeof(info), info, &bytesRead);
	if (error) {
		throw AdsException(error);
	}
	if (bytesRead < 24) {
		throw AdsException(ADSERR_DEVICE_INVALIDDATA);
	}
	const auto symbolSize = Le32(info + 4);
	const auto typeSize = Le32(info + 12);
	const auto codePage = bytesRead >= 32 ? Le32(info + 28) : 0;

	uint8_t version = 0;
	const auto versionError = device.ReadReqEx2(
		ADSIGRP_SYM_VERSION, 0, sizeof(version), &version, &bytesRead);
	if (versionError) {
		throw AdsException(versionError);
	}
	m_Version = version;
	const auto symbols = Upload(device, ADSIGRP_SYM_UPLOAD, symbolSize);
	const auto types = Upload(device, ADSIGRP_SYM_DT_UPLOAD, typeSize);
	m_Symbols = ParseSymbols(symbols.data(), symbols.size(), codePage);
	m_Types = ParseDataTypes(types.data(), types.size(), codePage);
	Index();
}

AdsSymbolTable::AdsSymbolTable(std::vector<AdsSymbol> symbols,
			       std::vector<AdsDataType> types, uint32_t version)
	: m_Symbols(std::move(symbols))
	, m_Types(std::move(types))
	, m_Version(version)
	, m_HasOnlineChanges(false)
	, m_OnlineChangesGroup(0)
	, m_OnlineChangesOffset(0)
	, m_OnlineChanges(0)
{
	Index();
}

void AdsSymbolTable::Index()
{
	m_TypeIndex.clear();
	m_SymbolIndex.clear();
	for (size_t i = 0; i < m_Types.size(); ++i) {
		m_TypeIndex.emplace_back(Lower(m_Types[i].name), i);
	}
	for (size_t i = 0; i < m_Symbols.size(); ++i) {
		m_SymbolIndex.emplace_back(Lower(m_Symbols[i].name), i);
	}
	std::sort(m_TypeIndex.begin(), m_TypeIndex.end());
	std::sort(m_SymbolIndex.begin(), m_SymbolIndex.end());
}

const std::vector<AdsSymbol> &AdsSymbolTable::Symbols() const
{
	return m_Symbols;
}

const std::vector<AdsDataType> &AdsSymbolTable::DataTypes() const
{
	return m_Types;
}

uint32_t AdsSymbolTable::Version() const
{
	return m_Version;
}

bool AdsSymbolTable::OnlineChanges(uint32_t &count) const
{
	if (m_HasOnlineChanges) {
		count = m_OnlineChanges;
	}
	return m_HasOnlineChanges;
}

const AdsDataType *AdsSymbolTable::FindType(const std::string &name) const
{
	const auto e = Find(m_TypeIndex, name);
	return e ? &m_Types[e->second] : nullptr;
}

const AdsSymbol *AdsSymbolTable::FindSymbol(const std::string &name) const
{
	const auto e = Find(m_SymbolIndex, name);
	return e ? &m_Symbols[e->second] : nullptr;
}

AdsElement AdsSymbolTable::Make(std::string path, const std::string &typeName,
				uint32_t indexGroup, uint32_t indexOffset,
				uint32_t size, uint32_t dataType,
				std::vector<AdsAttribute> attributes) const
{
	AdsElement e;
	e.path = std::move(path);
	e.typeName = typeName;
	e.indexGroup = indexGroup;
	e.indexOffset = indexOffset;
	e.size = size;
	e.bitOffset = 0;
	e.dataType = dataType;
	e.attributes = std::move(attributes);
	e.hasChildren = false;

	const auto declared = FindType(typeName);
	e.type = declared;
	if (StartsWith(typeName, "pointer to ")) {
		e.kind = AdsElement::Kind::Pointer;
		return e;
	}
	if (StartsWith(typeName, "reference to ")) {
		e.kind = AdsElement::Kind::Reference;
		return e;
	}
	if (HasAttribute(declared, "tcplcinterfacetype")) {
		e.kind = AdsElement::Kind::Interface;
		return e;
	}

	// An alias (TYPE T_Percent : REAL) describes nothing itself: follow it.
	auto t = declared;
	for (size_t i = 0; t && i < MAX_ALIAS_DEPTH; ++i) {
		if (!t->dims.empty() || !t->members.empty() ||
		    !t->enumValues.empty() || t->baseType.empty()) {
			break;
		}
		const auto base = FindType(t->baseType);
		if (!base || base == t) {
			break;
		}
		t = base;
	}
	e.type = t;
	const auto adsType = t ? t->dataType : dataType;
	if (t && !t->dims.empty()) {
		e.kind = AdsElement::Kind::Array;
		e.hasChildren = Elements(e) > 0;
	} else if (t && !t->enumValues.empty()) {
		e.kind = AdsElement::Kind::Enum;
	} else if (adsType == ADST_STRING) {
		e.kind = AdsElement::Kind::String;
	} else if (adsType == ADST_WSTRING) {
		e.kind = AdsElement::Kind::WString;
	} else if (t && !t->members.empty()) {
		e.kind = AdsElement::Kind::Struct;
		e.hasChildren = true;
	} else {
		e.kind = AdsElement::Kind::Value;
	}
	return e;
}

AdsElement AdsSymbolTable::Root(const AdsSymbol &symbol) const
{
	return Make(symbol.name, symbol.typeName, symbol.indexGroup,
		    symbol.indexOffset, symbol.size, symbol.dataType,
		    symbol.attributes);
}

uint64_t AdsSymbolTable::Elements(const AdsElement &array) const
{
	if (!array.type || array.type->dims.empty()) {
		return 0;
	}
	uint64_t n = 1;
	for (const auto &d : array.type->dims) {
		n *= d.elements;
	}
	return n;
}

std::vector<AdsElement> AdsSymbolTable::Children(const AdsElement &parent) const
{
	std::vector<AdsElement> children;
	if (!parent.hasChildren || !parent.type) {
		return children;
	}
	const auto &t = *parent.type;

	if (parent.kind == AdsElement::Kind::Array) {
		const auto total = Elements(parent);
		children.reserve(static_cast<size_t>(total));
		for (uint64_t k = 0; k < total; ++k) {
			children.push_back(ArrayElement(parent, k));
		}
		return children;
	}

	for (const auto &m : t.members) {
		auto path = parent.path + "." + m.name;
		if (m.property) {
			auto e = Make(std::move(path), m.typeName,
				      parent.indexGroup, 0, m.size, m.dataType,
				      m.attributes);
			e.kind = AdsElement::Kind::Property;
			e.hasChildren = false;
			children.push_back(std::move(e));
		} else if (m.offset == NO_OFFSET) {
			continue; // static member, no place inside the parent
		} else if (m.bitField) {
			auto e = Make(std::move(path), m.typeName,
				      parent.indexGroup,
				      parent.indexOffset + m.offset / 8, 1,
				      m.dataType, m.attributes);
			e.kind = AdsElement::Kind::BitField;
			e.bitOffset = m.offset % 8;
			e.hasChildren = false;
			children.push_back(std::move(e));
		} else {
			children.push_back(Make(std::move(path), m.typeName,
						parent.indexGroup,
						parent.indexOffset + m.offset,
						m.size, m.dataType,
						m.attributes));
		}
	}
	return children;
}

AdsElement AdsSymbolTable::ArrayElement(const AdsElement &array,
					 uint64_t k) const
{
	const auto &t = *array.type;
	const auto elementSize = static_cast<uint32_t>(t.size / Elements(array));
	const auto base = FindType(t.baseType);
	const auto dataType = base ? base->dataType : t.dataType;
	// Index of every dimension; the last dimension runs fastest.
	std::vector<int64_t> index(t.dims.size());
	auto rest = k;
	for (size_t d = t.dims.size(); d-- > 0;) {
		index[d] = t.dims[d].lowerBound +
			   static_cast<int64_t>(rest % t.dims[d].elements);
		rest /= t.dims[d].elements;
	}
	std::string path = array.path + "[";
	for (size_t d = 0; d < index.size(); ++d) {
		if (d) {
			path += ",";
		}
		path += std::to_string(index[d]);
	}
	path += "]";
	return Make(std::move(path), t.baseType, array.indexGroup,
		    array.indexOffset + static_cast<uint32_t>(k) * elementSize,
		    elementSize, dataType, array.attributes);
}

namespace
{
bool IsSpace(char c)
{
	return c == ' ' || c == '\t';
}

/** Reads "[i, j, ...]" at pos; false if it is not one */
bool ParseIndexes(const std::string &path, size_t &pos,
		  std::vector<int64_t> &indexes)
{
	indexes.clear();
	++pos; // '['
	for (;;) {
		while (pos < path.size() && IsSpace(path[pos])) {
			++pos;
		}
		const auto start = pos;
		if (pos < path.size() && (path[pos] == '-' || path[pos] == '+')) {
			++pos;
		}
		const auto digits = pos;
		int64_t value = 0;
		while (pos < path.size() && path[pos] >= '0' && path[pos] <= '9') {
			if (value > (INT64_MAX - 9) / 10) {
				return false;
			}
			value = value * 10 + (path[pos] - '0');
			++pos;
		}
		if (pos == digits) {
			return false;
		}
		indexes.push_back(path[start] == '-' ? -value : value);
		while (pos < path.size() && IsSpace(path[pos])) {
			++pos;
		}
		if (pos >= path.size()) {
			return false;
		}
		if (path[pos] == ']') {
			++pos;
			return true;
		}
		if (path[pos] != ',') {
			return false;
		}
		++pos;
	}
}
} // namespace

bool AdsSymbolTable::FindElement(const std::string &path,
				 AdsElement &element) const
{
	// Symbol names contain dots themselves ("GVL.var"): the longest
	// beginning of the path that is a symbol.
	const AdsSymbol *symbol = nullptr;
	size_t pos = path.size();
	for (;;) {
		symbol = FindSymbol(path.substr(0, pos));
		if (symbol || pos == 0) {
			break;
		}
		const auto cut = path.find_last_of(".[", pos - 1);
		pos = cut == std::string::npos ? 0 : cut;
	}
	if (!symbol) {
		return false;
	}
	auto e = Root(*symbol);
	std::vector<int64_t> indexes;
	while (pos < path.size()) {
		if (path[pos] == '[') {
			if (e.kind != AdsElement::Kind::Array || !e.type ||
			    !ParseIndexes(path, pos, indexes) ||
			    indexes.size() != e.type->dims.size()) {
				return false;
			}
			uint64_t k = 0;
			for (size_t d = 0; d < indexes.size(); ++d) {
				const auto &dim = e.type->dims[d];
				const auto i = indexes[d] - dim.lowerBound;
				if (i < 0 || i >= static_cast<int64_t>(dim.elements)) {
					return false;
				}
				k = k * dim.elements + static_cast<uint64_t>(i);
			}
			e = ArrayElement(e, k);
		} else if (path[pos] == '.' &&
			   e.kind != AdsElement::Kind::Array) {
			// Members only; the children of an array are elements.
			const auto end = path.find_first_of(".[", pos + 1);
			const auto name = Lower(e.path + path.substr(pos, end - pos));
			pos = end == std::string::npos ? path.size() : end;
			bool found = false;
			for (auto &c : Children(e)) {
				if (Lower(c.path) == name) {
					e = std::move(c);
					found = true;
					break;
				}
			}
			if (!found) {
				return false;
			}
		} else {
			return false;
		}
	}
	element = std::move(e);
	return true;
}

namespace
{
const size_t MAX_SUM_ENTRIES = 500;

// SumItem::element of the values read along with the elements to check
// the program: symbol version and number of online changes
const size_t VERSION_ITEM = SIZE_MAX;
const size_t ONLINE_CHANGES_ITEM = SIZE_MAX - 1;

struct SumItem {
	size_t element; // index into the elements and results, or *_ITEM
	uint32_t indexGroup;
	uint32_t indexOffset;
	uint32_t size;
};

/** What the program of a table looks like */
struct ProgramCheck {
	uint32_t version;
	bool hasOnlineChanges;
	uint32_t onlineChangesGroup;
	uint32_t onlineChangesOffset;
	uint32_t onlineChanges;
};

void Put32(std::vector<uint8_t> &b, uint32_t v)
{
	for (int i = 0; i < 4; ++i) {
		b.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}
}

/** Handles of properties, released when it goes */
struct Handles {
	const AdsDevice &device;
	std::vector<uint32_t> handles;

	~Handles()
	{
		for (auto h : handles) {
			device.WriteReqEx(ADSIGRP_SYM_RELEASEHND, 0, sizeof(h),
					  &h);
		}
	}
};

/** checks: results of VERSION_ITEM (0) and ONLINE_CHANGES_ITEM (1) */
void ReadSum(const AdsDevice &device, const std::vector<SumItem> &items,
	     size_t first, size_t count, std::vector<AdsReadResult> &results,
	     AdsReadResult (&checks)[2])
{
	std::vector<uint8_t> request;
	request.reserve(count * 12);
	size_t dataLength = 0;
	for (size_t i = first; i < first + count; ++i) {
		Put32(request, items[i].indexGroup);
		Put32(request, items[i].indexOffset);
		Put32(request, items[i].size);
		dataLength += items[i].size;
	}
	std::vector<uint8_t> answer(count * 4 + dataLength);
	uint32_t bytesRead = 0;
	const auto error = device.ReadWriteReqEx2(
		ADSIGRP_SUMUP_READ, static_cast<uint32_t>(count), answer.size(),
		answer.data(), request.size(), request.data(), &bytesRead);
	if (error) {
		throw AdsException(error);
	}
	if (bytesRead < count * 4) {
		throw AdsException(ADSERR_DEVICE_INVALIDDATA);
	}
	// First all error codes, then the data of every entry, failed or not.
	size_t data = count * 4;
	for (size_t i = 0; i < count; ++i) {
		const auto &item = items[first + i];
		auto &result = item.element == VERSION_ITEM ? checks[0] :
			       item.element == ONLINE_CHANGES_ITEM ?
							      checks[1] :
							      results[item.element];
		result.error = static_cast<long>(Le32(&answer[i * 4]));
		if (!result.error) {
			if (data + item.size > bytesRead) {
				result.error = ADSERR_DEVICE_INVALIDDATA;
			} else {
				result.data.assign(answer.begin() + data,
						   answer.begin() + data +
							   item.size);
			}
		}
		data += item.size;
	}
}
} // namespace

namespace
{
void AddChecks(std::vector<SumItem> &chunk, const ProgramCheck &check)
{
	chunk.push_back({ VERSION_ITEM, ADSIGRP_SYM_VERSION, 0, 1 });
	if (check.hasOnlineChanges) {
		chunk.push_back({ ONLINE_CHANGES_ITEM, check.onlineChangesGroup,
				  check.onlineChangesOffset, 4 });
	}
}

/** Whether the values read by AddChecks() are those of the table */
bool IsSameProgram(const AdsReadResult (&checks)[2], const ProgramCheck &check)
{
	const auto &version = checks[0];
	if (version.error || version.data.size() != 1) {
		throw AdsException(version.error ? version.error :
						   ADSERR_DEVICE_INVALIDDATA);
	}
	if (version.data[0] != check.version) {
		return false;
	}
	if (!check.hasOnlineChanges) {
		return true;
	}
	const auto &count = checks[1];
	if (count.error || count.data.size() != 4) {
		throw AdsException(count.error ? count.error :
						 ADSERR_DEVICE_INVALIDDATA);
	}
	return Le32(count.data.data()) == check.onlineChanges;
}

std::vector<AdsReadResult> Read(const AdsDevice &device,
				const std::vector<AdsElement> &elements,
				const ProgramCheck *check)
{
	std::vector<AdsReadResult> results(elements.size());
	std::vector<SumItem> items;
	items.reserve(elements.size());
	Handles handles{ device, {} };
	for (size_t i = 0; i < elements.size(); ++i) {
		const auto &e = elements[i];
		if (e.kind != AdsElement::Kind::Property) {
			items.push_back({ i, e.indexGroup, e.indexOffset, e.size });
			continue;
		}
		// A property has no address; the PLC calls its getter on reading by handle.
		uint32_t handle = 0;
		uint32_t bytesRead = 0;
		const auto error = device.ReadWriteReqEx2(
			ADSIGRP_SYM_HNDBYNAME, 0, sizeof(handle), &handle,
			e.path.size(), e.path.c_str(), &bytesRead);
		if (error) {
			results[i].error = error;
			continue;
		}
		handles.handles.push_back(handle);
		items.push_back({ i, ADSIGRP_SYM_VALBYHND, handle, e.size });
	}
	// With a table, every request first reads what identifies the
	// program: then no request can return data of another program
	// unnoticed.
	const size_t checkItems = !check ? 0 : check->hasOnlineChanges ? 2 : 1;
	const size_t perRequest = MAX_SUM_ENTRIES - checkItems;
	std::vector<SumItem> chunk;
	for (size_t first = 0; first < items.size(); first += perRequest) {
		const auto count = std::min(perRequest, items.size() - first);
		chunk.clear();
		if (check) {
			AddChecks(chunk, *check);
		}
		chunk.insert(chunk.end(), items.begin() + first,
			     items.begin() + first + count);
		AdsReadResult checks[2] = { { 0, {} }, { 0, {} } };
		ReadSum(device, chunk, 0, chunk.size(), results, checks);
		if (check && !IsSameProgram(checks, *check)) {
			throw AdsException(ADSERR_DEVICE_SYMBOLVERSIONINVALID);
		}
	}
	return results;
}
} // namespace

bool AdsSymbolTable::IsCurrent(const AdsDevice &device) const
{
	const ProgramCheck check{ m_Version, m_HasOnlineChanges,
				  m_OnlineChangesGroup, m_OnlineChangesOffset,
				  m_OnlineChanges };
	std::vector<SumItem> chunk;
	AddChecks(chunk, check);
	std::vector<AdsReadResult> none;
	AdsReadResult checks[2] = { { 0, {} }, { 0, {} } };
	ReadSum(device, chunk, 0, chunk.size(), none, checks);
	return IsSameProgram(checks, check);
}

std::vector<AdsReadResult> ReadElements(const AdsDevice &device,
					const std::vector<AdsElement> &elements)
{
	return Read(device, elements, nullptr);
}

std::vector<AdsReadResult> ReadElements(const AdsDevice &device,
					const AdsSymbolTable &table,
					const std::vector<AdsElement> &elements)
{
	const ProgramCheck check{ table.m_Version, table.m_HasOnlineChanges,
				  table.m_OnlineChangesGroup,
				  table.m_OnlineChangesOffset,
				  table.m_OnlineChanges };
	return Read(device, elements, &check);
}
