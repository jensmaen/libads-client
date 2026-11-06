// SPDX-License-Identifier: MIT
/**
   Copyright (c) 2026 Jens Fresenborg
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/**
 * Description of the variables of a PLC as its symbol and data type upload
 * delivers them (ADSIGRP_SYM_UPLOAD, ADSIGRP_SYM_DT_UPLOAD).
 *
 * The parse functions need no connection, so they work on recorded data as
 * well. Texts (names, types, comments, attributes) are UTF-8.
 */

/** Base type of a value as ADS reports it (dataType of an entry) */
enum AdsDataTypeId : uint32_t {
	ADST_VOID = 0,
	ADST_INT16 = 2,
	ADST_INT32 = 3,
	ADST_REAL32 = 4,
	ADST_REAL64 = 5,
	ADST_INT8 = 16,
	ADST_UINT8 = 17,
	ADST_UINT16 = 18,
	ADST_UINT32 = 19,
	ADST_INT64 = 20,
	ADST_UINT64 = 21,
	ADST_STRING = 30,
	ADST_WSTRING = 31,
	ADST_REAL80 = 32,
	ADST_BIT = 33,
	ADST_BIGTYPE = 65, // structs, function blocks, pointers, interfaces
};

/** One dimension of an array: ARRAY[lowerBound..lowerBound + elements - 1] */
struct AdsArrayDim {
	int32_t lowerBound;
	uint32_t elements;
};

/** One value of an enumeration */
struct AdsEnumValue {
	std::string name;
	int64_t value;
};

/** {attribute 'name' := 'value'} */
struct AdsAttribute {
	std::string name;
	std::string value;
};

/** Member of a struct, union, function block, GVL or program */
struct AdsDataMember {
	std::string name;
	std::string typeName;
	std::string comment;
	uint32_t offset; // bytes from the start of the parent; bits if bitField
	uint32_t size; // bytes; bits if bitField
	uint32_t dataType; // AdsDataTypeId
	uint32_t flags; // ADSDATATYPEFLAG_* as delivered
	bool bitField; // BIT member: offset and size count bits
	bool property; // property with monitoring: no memory, read it by name
	std::vector<AdsAttribute> attributes;
};

/** One data type of the upload */
struct AdsDataType {
	std::string name; // "ST_Machine", "STRING(31)", "ARRAY [1..4] OF REAL"
	std::string baseType; // alias, array element, enum base, pointee
	std::string comment;
	uint32_t size; // bytes
	uint32_t dataType; // AdsDataTypeId
	uint32_t flags; // ADSDATATYPEFLAG_* as delivered
	std::vector<AdsArrayDim> dims; // empty unless an array
	std::vector<AdsDataMember> members; // struct, union, FB (also properties)
	std::vector<AdsEnumValue> enumValues; // empty unless an enumeration
	std::vector<AdsAttribute> attributes;
};

/** One symbol of the upload, e.g. "GVL_Types.stMachine" */
struct AdsSymbol {
	std::string name;
	std::string typeName;
	std::string comment;
	uint32_t indexGroup;
	uint32_t indexOffset; // a bit address if bitAddress
	uint32_t size; // bytes
	uint32_t dataType; // AdsDataTypeId
	uint32_t flags; // ADSSYMBOLFLAG_* as delivered
	bool bitAddress; // e.g. AT %MX10.0: indexOffset counts bits
	bool persistent;
	bool reference; // REFERENCE TO: the value is the address
	std::vector<AdsAttribute> attributes;
};

/**
 * Takes apart the answer of ADSIGRP_SYM_UPLOAD.
 * @param[in] codePage of the texts as ADSIGRP_SYM_UPLOADINFO2 reports it;
 *            65001 (UTF-8) and 0 (unknown) leave them as they are,
 *            1252 is converted to UTF-8
 * @throw AdsException ADSERR_DEVICE_INVALIDDATA if the data is cut or broken
 */
std::vector<AdsSymbol> ParseSymbols(const uint8_t *data, size_t length,
				    uint32_t codePage = 65001);

/**
 * Takes apart the answer of ADSIGRP_SYM_DT_UPLOAD; see ParseSymbols().
 */
std::vector<AdsDataType> ParseDataTypes(const uint8_t *data, size_t length,
					uint32_t codePage = 65001);

/**
 * A symbol or a part of it down to a single value, e.g.
 * "GVL_Types.stMachine.aTemps[2]", with the address to read it.
 */
struct AdsElement {
	enum class Kind {
		Value, // number, BOOL, time, date …
		String, // STRING(n): bytes; see the attribute TcEncoding
		WString, // WSTRING(n): UTF-16
		Enum, // type->enumValues names the values
		Struct, // struct, union, function block: children are members
		Array, // children are the elements
		Pointer, // POINTER TO: the value is an address
		Reference, // REFERENCE TO: the value is an address
		Interface, // the value is an address
		Property, // property with monitoring: no address, read by path
		BitField, // BIT member: bit bitOffset of the byte at indexOffset
	};

	std::string path;
	std::string typeName;
	const AdsDataType *type; // nullptr if the upload has no such type
	uint32_t indexGroup;
	uint32_t indexOffset;
	uint32_t size; // bytes
	uint32_t bitOffset; // only for BitField
	uint32_t dataType; // AdsDataTypeId
	Kind kind;
	bool hasChildren;
	std::vector<AdsAttribute> attributes; // of the symbol or member
};

struct AdsDevice;
struct AdsReadResult;

/**
 * Symbols and data types of a PLC, uploaded once. Elements point into the
 * table, so it must outlive them; it can be moved, not copied.
 */
class AdsSymbolTable {
    public:
	/**
	 * Uploads symbols and data types from the device (usually port 851).
	 * @throw AdsException on any error of the upload or broken data
	 */
	explicit AdsSymbolTable(const AdsDevice &device);

	/** From an upload taken apart before, e.g. recorded data */
	AdsSymbolTable(std::vector<AdsSymbol> symbols,
		       std::vector<AdsDataType> types, uint32_t version = 0);

	AdsSymbolTable(const AdsSymbolTable &) = delete;
	AdsSymbolTable &operator=(const AdsSymbolTable &) = delete;
	AdsSymbolTable(AdsSymbolTable &&) = default;
	AdsSymbolTable &operator=(AdsSymbolTable &&) = default;

	const std::vector<AdsSymbol> &Symbols() const;
	const std::vector<AdsDataType> &DataTypes() const;

	/** Symbol version at the upload (ADSIGRP_SYM_VERSION); it changes
	    when the PLC gets a new program by a download */
	uint32_t Version() const;

	/**
	 * Number of online changes at the upload
	 * (TwinCAT_SystemInfoVarList._AppInfo.OnlineChangeCnt). An online
	 * change keeps the symbol version, so the table checks this number
	 * as well. False if the PLC has no such value; then only the version
	 * is checked.
	 */
	bool OnlineChanges(uint32_t &count) const;

	/**
	 * Whether the PLC still has the program of this table: reads the
	 * symbol version and the number of online changes of the device and
	 * compares them with those of the upload.
	 * @throw AdsException if they cannot be read
	 */
	bool IsCurrent(const AdsDevice &device) const;

	/** Names are compared without regard to case, as in the PLC.
	    @return nullptr if there is none */
	const AdsDataType *FindType(const std::string &name) const;
	const AdsSymbol *FindSymbol(const std::string &name) const;

	/** The element of a whole symbol */
	AdsElement Root(const AdsSymbol &symbol) const;

	/** One level down: members of a struct, elements of an array */
	std::vector<AdsElement> Children(const AdsElement &parent) const;

	/**
	 * The element at a path such as "GVL.aMachines[2].stAxis.fPos",
	 * "GVL.aMatrix[1,2]" or "GVL.aNested[0][2]", without regard to case.
	 * Spaces are allowed around indexes.
	 * @return false if there is no such element
	 */
	bool FindElement(const std::string &path, AdsElement &element) const;

	/**
	 * Calls f for every element without children, depth first, symbol by
	 * symbol. An array with more than maxArray elements is not expanded
	 * and handed to f as a whole.
	 */
	template <typename F>
	void ForEachLeaf(F &&f, uint32_t maxArray = 1000) const
	{
		for (const auto &s : m_Symbols) {
			Visit(Root(s), f, maxArray);
		}
	}

    private:
	template <typename F>
	void Visit(const AdsElement &e, F &f, uint32_t maxArray) const
	{
		if (!e.hasChildren || (e.kind == AdsElement::Kind::Array &&
				       Elements(e) > maxArray)) {
			f(e);
			return;
		}
		for (const auto &c : Children(e)) {
			Visit(c, f, maxArray);
		}
	}

	uint64_t Elements(const AdsElement &array) const;
	AdsElement ArrayElement(const AdsElement &array, uint64_t k) const;
	AdsElement Make(std::string path, const std::string &typeName,
			uint32_t indexGroup, uint32_t indexOffset,
			uint32_t size, uint32_t dataType,
			std::vector<AdsAttribute> attributes) const;
	void Index();
	void Load(const AdsDevice &device);

	friend std::vector<AdsReadResult>
	ReadElements(const AdsDevice &device, const AdsSymbolTable &table,
		     const std::vector<AdsElement> &elements);

	std::vector<AdsSymbol> m_Symbols;
	std::vector<AdsDataType> m_Types;
	std::vector<std::pair<std::string, size_t> > m_TypeIndex; // sorted
	std::vector<std::pair<std::string, size_t> > m_SymbolIndex; // sorted
	uint32_t m_Version;
	// Address and value of OnlineChangeCnt, if the PLC has it
	bool m_HasOnlineChanges;
	uint32_t m_OnlineChangesGroup;
	uint32_t m_OnlineChangesOffset;
	uint32_t m_OnlineChanges;
};

/** Value of one element as read, or why not */
struct AdsReadResult {
	long error; // ADS error code of this element, 0 if read
	std::vector<uint8_t> data; // size bytes of the element if read
};

/**
 * Reads many elements with few requests (ADSIGRP_SUMUP_READ, at most 500
 * elements each). Properties are read through a handle by their path. A
 * BitField comes as its whole byte.
 * @return one result per element, in the same order
 * @throw AdsException if a request as a whole fails (e.g. timeout)
 */
std::vector<AdsReadResult> ReadElements(const AdsDevice &device,
					const std::vector<AdsElement> &elements);

/**
 * As above, and makes sure the elements are still those of the PLC's
 * program: the symbol version and the number of online changes are read
 * in the same requests and compared with those of the table.
 * @throw AdsException with ADSERR_DEVICE_SYMBOLVERSIONINVALID if the PLC
 *        got a new program (download or online change) since the upload;
 *        upload a new table then
 */
std::vector<AdsReadResult> ReadElements(const AdsDevice &device,
					const AdsSymbolTable &table,
					const std::vector<AdsElement> &elements);
