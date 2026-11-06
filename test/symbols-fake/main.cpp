// Symbol and data type uploads taken apart (AdsSymbols.h), on data built after the format of TwinCAT; no TwinCAT
// and no network needed.

#include "AdsSymbols.h"
#include "AdsException.h"
#include "AdsDef.h"
#include "UploadBuilder.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "[ ok ]" : "[FAIL]", what.c_str());
    if (!ok) ++failures;
}

// The error code the function throws, 0 if it does not throw
long Thrown(const std::function<void()>& f) {
    try {
        f();
    } catch (const AdsException& e) {
        return e.errorCode;
    }
    return 0;
}

std::vector<upload::Symbol> TestSymbols() {
    upload::Symbol machine;
    machine.name = "GVL.stMachine";
    machine.type = "ST_Machine";
    machine.comment = "the machine";
    machine.offset = 0x5eb98;
    machine.size = 88;

    upload::Symbol bit;
    bit.name = "GVL.xBit0";
    bit.type = "BOOL";
    bit.group = 0x4021;
    bit.offset = 80; // %MX10.0
    bit.size = 1;
    bit.dataType = ADST_BIT;
    bit.flags |= 0x2;

    upload::Symbol utf8;
    utf8.name = "GVL.sUtf8";
    utf8.type = "STRING(80)";
    utf8.size = 81;
    utf8.dataType = ADST_STRING;
    utf8.attributes = {{"TcEncoding", "UTF-8"}};

    upload::Symbol ref;
    ref.name = "GVL.refMachine";
    ref.type = "REFERENCE TO ST_Machine";
    ref.size = 8;
    ref.flags |= 0x4;
    ref.extendedFlags = true;

    upload::Symbol persistent;
    persistent.name = "GVL_Persistent.nCounter";
    persistent.type = "UDINT";
    persistent.size = 4;
    persistent.dataType = ADST_UINT32;
    persistent.flags |= 0x1;
    return {machine, bit, utf8, ref, persistent};
}

std::vector<upload::Type> TestTypes() {
    upload::Type flags;
    flags.name = "ST_Flags";
    flags.size = 2;
    for (int i = 0; i < 3; ++i) {
        upload::Type m;
        m.name = std::string("b") + static_cast<char>('A' + i);
        m.type = "BIT";
        m.size = 1;
        m.offset = static_cast<uint32_t>(i);
        m.dataType = ADST_BIT;
        m.flags |= 0x20;
        m.attributes = {{"DisplayMinValue", "0"}};
        flags.members.push_back(m);
    }
    upload::Type after;
    after.name = "nAfter";
    after.type = "BYTE";
    after.size = 1;
    after.offset = 1;
    after.dataType = ADST_UINT8;
    flags.members.push_back(after);

    upload::Type counter;
    counter.name = "FB_Counter";
    counter.size = 24;
    counter.attributes = {{"PouType", "FunctionBlock"}};
    counter.methods = 2;
    upload::Type count;
    count.name = "nCount";
    count.type = "DINT";
    count.size = 4;
    count.offset = 12;
    count.dataType = ADST_INT32;
    upload::Type doubled = count;
    doubled.name = "Doubled";
    doubled.offset = 0;
    doubled.flags |= 0x40;
    upload::Type nested = count; // a member with members of its own, which the parser must step over
    nested.name = "stInner";
    nested.type = "ST_Inner";
    nested.members = {count, count};
    nested.offset = 16;
    counter.members = {count, doubled, nested};

    upload::Type mode;
    mode.name = "E_Mode";
    mode.type = "UDINT";
    mode.size = 4;
    mode.dataType = ADST_UINT32;
    mode.enumValues = {{"Off", 0}, {"Manual", 10}, {"Huge", 0xFFFFFFF0}};

    upload::Type color;
    color.name = "E_Color";
    color.type = "INT";
    color.size = 2;
    color.dataType = ADST_INT16;
    color.enumValues = {{"Minus", -1}, {"Green", 1}};

    upload::Type wide;
    wide.name = "E_Wide";
    wide.type = "LINT";
    wide.size = 8;
    wide.dataType = ADST_INT64;
    wide.enumValues = {{"Min", INT64_MIN}, {"Max", INT64_MAX}};

    upload::Type offsetArray;
    offsetArray.name = "ARRAY [-2..2] OF INT";
    offsetArray.type = "INT";
    offsetArray.size = 10;
    offsetArray.dataType = ADST_INT16;
    offsetArray.dims = {{-2, 5}};

    upload::Type cube;
    cube.name = "ARRAY [1..2,1..3,1..4] OF BYTE";
    cube.type = "BYTE";
    cube.size = 24;
    cube.dataType = ADST_UINT8;
    cube.dims = {{1, 2}, {1, 3}, {1, 4}};

    upload::Type future; // unknown flag and unknown bytes behind the known parts
    future.name = "T_Future";
    future.type = "REAL";
    future.size = 4;
    future.dataType = ADST_REAL32;
    future.flags |= 0x800000;
    future.copyMask = {0xFF, 0xFF, 0xFF, 0xFF};
    future.unknownTail = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    return {flags, counter, mode, color, wide, offsetArray, cube, future};
}

void Symbols() {
    const auto data = upload::Join(TestSymbols());
    std::vector<AdsSymbol> s;
    const long err = Thrown([&] { s = ParseSymbols(data.data(), data.size()); });
    check(err == 0 && s.size() == 5, "symbols: 5 taken apart");
    if (s.size() != 5) return;
    check(s[0].name == "GVL.stMachine" && s[0].typeName == "ST_Machine" && s[0].comment == "the machine" &&
              s[0].indexGroup == 0x4040 && s[0].indexOffset == 0x5eb98 && s[0].size == 88 &&
              s[0].dataType == ADST_BIGTYPE && !s[0].bitAddress && !s[0].reference && s[0].attributes.empty(),
          "symbols: struct with name, type, comment, group, offset, size");
    check(s[1].bitAddress && s[1].indexGroup == 0x4021 && s[1].indexOffset == 80, "symbols: bit address %MX10.0");
    check(s[2].attributes.size() == 1 && s[2].attributes[0].name == "TcEncoding" &&
              s[2].attributes[0].value == "UTF-8",
          "symbols: attribute TcEncoding");
    check(s[3].reference && s[3].typeName == "REFERENCE TO ST_Machine", "symbols: reference with extended flags");
    check(s[4].persistent && s[4].dataType == ADST_UINT32, "symbols: persistent");

    auto padded = data;
    padded.insert(padded.end(), 16, 0);
    check(ParseSymbols(padded.data(), padded.size()).size() == 5, "symbols: zeros behind the last entry");
    check(ParseSymbols(nullptr, 0).empty(), "symbols: empty upload");
}

void DataTypes() {
    const auto data = upload::Join(TestTypes());
    std::vector<AdsDataType> t;
    const long err = Thrown([&] { t = ParseDataTypes(data.data(), data.size()); });
    check(err == 0 && t.size() == 8, "types: 8 taken apart");
    if (t.size() != 8) return;

    const auto& flags = t[0];
    check(flags.name == "ST_Flags" && flags.size == 2 && flags.members.size() == 4, "types: struct with 4 members");
    if (flags.members.size() == 4) {
        bool bits = true;
        for (int i = 0; i < 3; ++i) {
            const auto& m = flags.members[i];
            bits = bits && m.bitField && m.offset == static_cast<uint32_t>(i) && m.size == 1 &&
                   m.typeName == "BIT" && m.attributes.size() == 1;
        }
        check(bits, "types: BIT members with offset and size in bits, attributes");
        check(!flags.members[3].bitField && flags.members[3].offset == 1, "types: byte member behind the bits");
    }

    const auto& counter = t[1];
    check(counter.members.size() == 3 && counter.attributes.size() == 1 &&
              counter.attributes[0].value == "FunctionBlock",
          "types: FB with methods skipped, attribute read");
    if (counter.members.size() == 3) {
        check(!counter.members[0].property && counter.members[0].offset == 12, "types: FB member");
        check(counter.members[1].property && counter.members[1].name == "Doubled", "types: property");
        check(counter.members[2].name == "stInner" && counter.members[2].offset == 16,
              "types: member with members of its own stepped over");
    }

    check(t[2].baseType == "UDINT" && t[2].enumValues.size() == 3 && t[2].enumValues[1].name == "Manual" &&
              t[2].enumValues[1].value == 10 && t[2].enumValues[2].value == 0xFFFFFFF0,
          "types: enum on UDINT, unsigned values");
    check(t[3].enumValues.size() == 2 && t[3].enumValues[0].value == -1, "types: enum on INT, negative value");
    check(t[4].enumValues.size() == 2 && t[4].enumValues[0].value == INT64_MIN &&
              t[4].enumValues[1].value == INT64_MAX,
          "types: enum on LINT");
    check(t[5].dims.size() == 1 && t[5].dims[0].lowerBound == -2 && t[5].dims[0].elements == 5 &&
              t[5].baseType == "INT",
          "types: array with lower bound -2");
    check(t[6].dims.size() == 3 && t[6].dims[2].lowerBound == 1 && t[6].dims[2].elements == 4,
          "types: array with 3 dimensions");
    check(t[7].name == "T_Future" && t[7].baseType == "REAL" && t[7].size == 4,
          "types: copy mask, unknown flag and unknown bytes skipped");
}

void CodePage() {
    upload::Symbol s;
    s.name = "GVL.fTemp";
    s.type = "REAL";
    s.comment = "Temperatur in \xB0" "C, R\xFC" "cklauf \x80";
    const auto data = upload::Join(std::vector<upload::Symbol>{s});
    const auto converted = ParseSymbols(data.data(), data.size(), 1252);
    check(converted.size() == 1 && converted[0].comment == "Temperatur in \xC2\xB0" "C, R\xC3\xBC" "cklauf \xE2\x82\xAC",
          "code page 1252 converted to UTF-8");
    const auto kept = ParseSymbols(data.data(), data.size(), 65001);
    check(kept.size() == 1 && kept[0].comment == s.comment, "code page 65001 kept as it is");
}

// Every cut of the data and every broken length must end in ADSERR_DEVICE_INVALIDDATA, never in a crash.
void Broken() {
    const auto symbols = upload::Join(TestSymbols());
    const auto types = upload::Join(TestTypes());
    int wrong = 0;
    for (size_t n = 0; n < symbols.size(); ++n) {
        const std::vector<uint8_t> cut(symbols.begin(), symbols.begin() + static_cast<long>(n));
        const long err = Thrown([&] { ParseSymbols(cut.data(), cut.size()); });
        if (err != 0 && err != ADSERR_DEVICE_INVALIDDATA) ++wrong;
    }
    for (size_t n = 0; n < types.size(); ++n) {
        const std::vector<uint8_t> cut(types.begin(), types.begin() + static_cast<long>(n));
        const long err = Thrown([&] { ParseDataTypes(cut.data(), cut.size()); });
        if (err != 0 && err != ADSERR_DEVICE_INVALIDDATA) ++wrong;
    }
    check(wrong == 0, "every cut of the data: taken apart or 0x70B");

    // A cut in the middle of an entry is always an error.
    const size_t first = symbols[0] | symbols[1] << 8 | symbols[2] << 16 | static_cast<size_t>(symbols[3]) << 24;
    const std::vector<uint8_t> half(symbols.begin(), symbols.begin() + static_cast<long>(first / 2));
    check(Thrown([&] { ParseSymbols(half.data(), half.size()); }) == ADSERR_DEVICE_INVALIDDATA,
          "entry cut in the middle: 0x70B");

    auto bad = symbols;
    bad[0] = 10, bad[1] = bad[2] = bad[3] = 0;
    check(Thrown([&] { ParseSymbols(bad.data(), bad.size()); }) == ADSERR_DEVICE_INVALIDDATA,
          "entry length below the header: 0x70B");
    bad = symbols;
    bad[3] = 0x7F;
    check(Thrown([&] { ParseSymbols(bad.data(), bad.size()); }) == ADSERR_DEVICE_INVALIDDATA,
          "entry length beyond the data: 0x70B");
    bad = symbols;
    bad[24] = 0xFF; // name length
    check(Thrown([&] { ParseSymbols(bad.data(), bad.size()); }) == ADSERR_DEVICE_INVALIDDATA,
          "name longer than the entry: 0x70B");
    bad = symbols;
    bad[30 + 13] = 'X'; // terminating zero of "GVL.stMachine"
    check(Thrown([&] { ParseSymbols(bad.data(), bad.size()); }) == ADSERR_DEVICE_INVALIDDATA,
          "missing terminating zero: 0x70B");

    auto badTypes = types;
    badTypes[40] = 0xFF; // number of members of ST_Flags
    check(Thrown([&] { ParseDataTypes(badTypes.data(), badTypes.size()); }) == ADSERR_DEVICE_INVALIDDATA,
          "more members than the entry holds: 0x70B");
}

upload::Type Member(const std::string& name, const std::string& type, uint32_t offset, uint32_t size,
                    uint32_t dataType = ADST_BIGTYPE) {
    upload::Type m;
    m.name = name;
    m.type = type;
    m.offset = offset;
    m.size = size;
    m.dataType = dataType;
    return m;
}

upload::Type Plain(const std::string& name, const std::string& base, uint32_t size, uint32_t dataType) {
    upload::Type t;
    t.name = name;
    t.type = base;
    t.size = size;
    t.dataType = dataType;
    return t;
}

upload::Type ArrayOf(const std::string& name, const std::string& base, uint32_t size, uint32_t dataType,
                     std::vector<std::pair<int32_t, uint32_t>> dims) {
    auto t = Plain(name, base, size, dataType);
    t.dims = std::move(dims);
    return t;
}

upload::Symbol Sym(const std::string& name, const std::string& type, uint32_t offset, uint32_t size,
                   uint32_t dataType = ADST_BIGTYPE) {
    upload::Symbol s;
    s.name = name;
    s.type = type;
    s.offset = offset;
    s.size = size;
    s.dataType = dataType;
    return s;
}

// A small PLC: structs with BIT members and a property, an alias, arrays of every kind, enum, string, pointer,
// reference, interface.
AdsSymbolTable TestTable() {
    upload::Type inner;
    inner.name = "ST_Inner";
    inner.size = 4;
    auto bitA = Member("bA", "BIT", 0, 1, ADST_BIT);
    bitA.flags |= 0x20;
    auto bitB = Member("bB", "BIT", 9, 1, ADST_BIT); // second byte, bit 1
    bitB.flags |= 0x20;
    inner.members = {bitA, bitB, Member("nValue", "INT", 2, 2, ADST_INT16)};

    upload::Type outer;
    outer.name = "ST_Outer";
    outer.size = 16;
    auto prop = Member("Doubled", "DINT", 0, 4, ADST_INT32);
    prop.flags |= 0x40;
    auto staticMember = Member("nStatic", "DINT", 0xFFFFFFFF, 4, ADST_INT32);
    outer.members = {Member("nId", "UDINT", 0, 4, ADST_UINT32), Member("stInner", "ST_Inner", 4, 4),
                     Member("aInner", "ARRAY [1..2] OF ST_Inner", 8, 8), prop, staticMember};

    upload::Type shape = Plain("I_Shape", "", 8, ADST_BIGTYPE);
    shape.attributes = {{"TcPlcInterfaceType", ""}};

    upload::Type color = Plain("E_Color", "INT", 2, ADST_INT16);
    color.enumValues = {{"Red", 0}, {"Green", 1}};

    const std::vector<upload::Type> types = {
        inner,
        outer,
        Plain("T_Outer", "ST_Outer", 16, ADST_BIGTYPE), // alias of a struct
        Plain("T_Alias2", "T_Outer", 16, ADST_BIGTYPE), // alias of an alias
        Plain("INT", "", 2, ADST_INT16),
        Plain("UDINT", "", 4, ADST_UINT32),
        Plain("BIT", "", 1, ADST_BIT),
        Plain("STRING(5)", "", 6, ADST_STRING),
        Plain("WSTRING(3)", "", 8, ADST_WSTRING),
        Plain("POINTER TO DINT", "DINT", 8, ADST_BIGTYPE),
        Plain("REFERENCE TO ST_Outer", "ST_Outer", 8, ADST_BIGTYPE),
        shape,
        color,
        ArrayOf("ARRAY [1..2] OF ST_Inner", "ST_Inner", 8, ADST_BIGTYPE, {{1, 2}}),
        ArrayOf("ARRAY [-2..2] OF INT", "INT", 10, ADST_INT16, {{-2, 5}}),
        ArrayOf("ARRAY [0..1,0..2] OF INT", "INT", 12, ADST_INT16, {{0, 2}, {0, 3}}),
        ArrayOf("ARRAY [0..2] OF INT", "INT", 6, ADST_INT16, {{0, 3}}),
        ArrayOf("ARRAY [0..1] OF ARRAY [0..2] OF INT", "ARRAY [0..2] OF INT", 12, ADST_INT16, {{0, 2}}),
        ArrayOf("ARRAY [1..2] OF STRING(5)", "STRING(5)", 12, ADST_STRING, {{1, 2}}),
        ArrayOf("ARRAY [0..1999] OF UDINT", "UDINT", 8000, ADST_UINT32, {{0, 2000}}),
    };
    auto pointer = Sym("GVL.pDint", "POINTER TO DINT", 0x500, 8);
    auto reference = Sym("GVL.refOuter", "REFERENCE TO ST_Outer", 0x508, 8);
    reference.flags |= 0x4;
    const std::vector<upload::Symbol> symbols = {
        Sym("GVL.stOuter", "ST_Outer", 0x100, 16),
        Sym("GVL.alias", "T_Alias2", 0x200, 16),
        Sym("GVL.aOffset", "ARRAY [-2..2] OF INT", 0x300, 10, ADST_INT16),
        Sym("GVL.aMatrix", "ARRAY [0..1,0..2] OF INT", 0x320, 12, ADST_INT16),
        Sym("GVL.aNested", "ARRAY [0..1] OF ARRAY [0..2] OF INT", 0x340, 12, ADST_INT16),
        Sym("GVL.aTexts", "ARRAY [1..2] OF STRING(5)", 0x360, 12, ADST_STRING),
        Sym("GVL.eColor", "E_Color", 0x380, 2, ADST_INT16),
        Sym("GVL.wsText", "WSTRING(3)", 0x390, 8, ADST_WSTRING),
        Sym("GVL.iShape", "I_Shape", 0x3A0, 8),
        pointer,
        reference,
        Sym("GVL.aLarge", "ARRAY [0..1999] OF UDINT", 0x1000, 8000, ADST_UINT32),
        Sym("GVL.nUnknown", "T_NotInUpload", 0x600, 4, ADST_UINT32),
    };
    const auto s = upload::Join(symbols);
    const auto t = upload::Join(types);
    return AdsSymbolTable(ParseSymbols(s.data(), s.size()), ParseDataTypes(t.data(), t.size()), 7);
}

const AdsElement* ByPath(const std::vector<AdsElement>& elements, const std::string& path) {
    for (const auto& e : elements) {
        if (e.path == path) return &e;
    }
    return nullptr;
}

void Table() {
    const auto table = TestTable();
    check(table.Symbols().size() == 13 && table.Version() == 7, "table: 13 symbols, version kept");
    check(table.FindSymbol("gvl.STOUTER") && table.FindType("st_outer") && !table.FindSymbol("GVL.none"),
          "table: names found without regard to case");

    const auto outer = table.Root(*table.FindSymbol("GVL.stOuter"));
    check(outer.kind == AdsElement::Kind::Struct && outer.hasChildren && outer.indexOffset == 0x100,
          "table: struct symbol");
    const auto members = table.Children(outer);
    check(members.size() == 4, "table: members of a struct, static member left out");
    const auto inner = ByPath(members, "GVL.stOuter.stInner");
    check(inner && inner->kind == AdsElement::Kind::Struct && inner->indexOffset == 0x104,
          "table: nested struct at parent offset + member offset");
    if (inner) {
        const auto bits = table.Children(*inner);
        const auto a = ByPath(bits, "GVL.stOuter.stInner.bA");
        const auto b = ByPath(bits, "GVL.stOuter.stInner.bB");
        check(a && a->kind == AdsElement::Kind::BitField && a->indexOffset == 0x104 && a->bitOffset == 0 &&
                  b && b->indexOffset == 0x105 && b->bitOffset == 1,
              "table: BIT members as byte and bit");
    }
    const auto prop = ByPath(members, "GVL.stOuter.Doubled");
    check(prop && prop->kind == AdsElement::Kind::Property && !prop->hasChildren, "table: property");
    const auto arr = ByPath(members, "GVL.stOuter.aInner");
    if (arr) {
        const auto elements = table.Children(*arr);
        check(elements.size() == 2 && elements[1].path == "GVL.stOuter.aInner[2]" &&
                  elements[1].indexOffset == 0x10C && elements[1].kind == AdsElement::Kind::Struct,
              "table: array of struct inside a struct");
    } else {
        check(false, "table: array of struct inside a struct");
    }

    const auto alias = table.Root(*table.FindSymbol("GVL.alias"));
    check(alias.kind == AdsElement::Kind::Struct && alias.typeName == "T_Alias2" && alias.type &&
              alias.type->name == "ST_Outer" && table.Children(alias).size() == 4,
          "table: alias of an alias of a struct followed");

    const auto offset = table.Children(table.Root(*table.FindSymbol("GVL.aOffset")));
    check(offset.size() == 5 && offset[0].path == "GVL.aOffset[-2]" && offset[0].indexOffset == 0x300 &&
              offset[4].path == "GVL.aOffset[2]" && offset[4].indexOffset == 0x308 && offset[4].size == 2,
          "table: array with lower bound -2");
    const auto matrix = table.Children(table.Root(*table.FindSymbol("GVL.aMatrix")));
    check(matrix.size() == 6 && matrix[5].path == "GVL.aMatrix[1,2]" && matrix[5].indexOffset == 0x320 + 10 &&
              matrix[1].path == "GVL.aMatrix[0,1]",
          "table: 2 dimensions, the last runs fastest");
    const auto nested = table.Children(table.Root(*table.FindSymbol("GVL.aNested")));
    if (nested.size() == 2) {
        const auto deep = table.Children(nested[1]);
        check(nested[1].kind == AdsElement::Kind::Array && deep.size() == 3 &&
                  deep[2].path == "GVL.aNested[1][2]" && deep[2].indexOffset == 0x340 + 10,
              "table: array of array");
    } else {
        check(false, "table: array of array");
    }
    const auto texts = table.Children(table.Root(*table.FindSymbol("GVL.aTexts")));
    check(texts.size() == 2 && texts[1].kind == AdsElement::Kind::String && texts[1].size == 6 &&
              texts[1].indexOffset == 0x366,
          "table: array of STRING");

    const auto kindOf = [&](const char* name) { return table.Root(*table.FindSymbol(name)).kind; };
    check(kindOf("GVL.eColor") == AdsElement::Kind::Enum, "table: enum");
    check(kindOf("GVL.wsText") == AdsElement::Kind::WString, "table: WSTRING");
    check(kindOf("GVL.iShape") == AdsElement::Kind::Interface, "table: interface");
    check(kindOf("GVL.pDint") == AdsElement::Kind::Pointer, "table: pointer");
    check(kindOf("GVL.refOuter") == AdsElement::Kind::Reference && !table.Root(*table.FindSymbol("GVL.refOuter")).hasChildren,
          "table: reference not followed");
    const auto unknown = table.Root(*table.FindSymbol("GVL.nUnknown"));
    check(unknown.kind == AdsElement::Kind::Value && !unknown.type, "table: type missing in the upload");

    std::vector<std::string> leaves;
    table.ForEachLeaf([&](const AdsElement& e) { leaves.push_back(e.path); });
    const auto has = [&](const char* p) { return std::find(leaves.begin(), leaves.end(), p) != leaves.end(); };
    check(has("GVL.stOuter.aInner[2].nValue") && has("GVL.aNested[1][2]") && has("GVL.aLarge") &&
              !has("GVL.aLarge[0]") && !has("GVL.stOuter"),
          "ForEachLeaf: down to single values, large array as a whole");
    leaves.clear();
    table.ForEachLeaf([&](const AdsElement& e) { leaves.push_back(e.path); }, 5000);
    check(has("GVL.aLarge[1999]"), "ForEachLeaf: large array expanded with a higher limit");
}

void Find() {
    const auto table = TestTable();
    AdsElement e;
    const auto found = [&](const char* path) { return table.FindElement(path, e); };
    check(found("GVL.stOuter") && e.path == "GVL.stOuter" && e.indexOffset == 0x100, "find: symbol");
    check(found("gvl.STOUTER.aInner[2].NVALUE") && e.path == "GVL.stOuter.aInner[2].nValue" &&
              e.indexOffset == 0x100 + 8 + 4 + 2 && e.size == 2,
          "find: member of an array element, path as in the PLC");
    check(found("GVL.stOuter.stInner.bB") && e.kind == AdsElement::Kind::BitField && e.indexOffset == 0x105 &&
              e.bitOffset == 1,
          "find: BIT member");
    check(found("GVL.stOuter.Doubled") && e.kind == AdsElement::Kind::Property, "find: property");
    check(found("GVL.aOffset[-2]") && e.indexOffset == 0x300 && found("GVL.aOffset[ 2 ]") && e.indexOffset == 0x308,
          "find: array with negative lower bound");
    check(found("GVL.aMatrix[1,2]") && e.path == "GVL.aMatrix[1,2]" && e.indexOffset == 0x320 + 10 &&
              found("GVL.aMatrix[0, 1]") && e.indexOffset == 0x322,
          "find: two dimensions");
    check(found("GVL.aNested[1][2]") && e.indexOffset == 0x340 + 10 && e.size == 2, "find: array of array");
    check(found("GVL.aLarge[1999]") && e.indexOffset == 0x1000 + 7996, "find: last element of a large array");
    check(found("GVL.alias.stInner.nValue") && e.indexOffset == 0x206, "find: through an alias");
    check(!found("GVL.none") && !found("GVL.stOuter.nNone") && !found("GVL.stOuter.nStatic") &&
              !found("GVL.aOffset[3]") && !found("GVL.aOffset[-3]") && !found("GVL.aMatrix[1]") &&
              !found("GVL.aMatrix[1][2]") && !found("GVL.aOffset[x]") && !found("GVL.aOffset[1") &&
              !found("GVL.aOffset.x") && !found("GVL.stOuter[1]") && !found("GVL.eColor.x") &&
              !found("GVL.refOuter.nId") && !found("") && !found("GVL.stOuter."),
          "find: wrong paths give false");
}

std::vector<uint8_t> ReadFile(const char* path) {
    std::vector<uint8_t> data;
    if (FILE* f = std::fopen(path, "rb")) {
        uint8_t buffer[4096];
        size_t n;
        while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) data.insert(data.end(), buffer, buffer + n);
        std::fclose(f);
    }
    return data;
}

// Takes apart an upload saved from a real PLC (answers of 0xF00B and 0xF00E) and prints it.
int Recorded(const char* symbolFile, const char* typeFile) {
    const auto symbolData = ReadFile(symbolFile);
    const auto typeData = ReadFile(typeFile);
    try {
        for (const auto& s : ParseSymbols(symbolData.data(), symbolData.size())) {
            std::printf("symbol %-45s %-30s 0x%X:0x%X size %u%s%s%s", s.name.c_str(), s.typeName.c_str(),
                        s.indexGroup, s.indexOffset, s.size, s.bitAddress ? " bit" : "",
                        s.persistent ? " persistent" : "", s.reference ? " reference" : "");
            for (const auto& a : s.attributes) std::printf(" {%s=%s}", a.name.c_str(), a.value.c_str());
            std::printf("\n");
        }
        for (const auto& t : ParseDataTypes(typeData.data(), typeData.size())) {
            std::printf("type %-35s base %-20s size %u dims %zu members %zu enums %zu attributes %zu\n",
                        t.name.c_str(), t.baseType.c_str(), t.size, t.dims.size(), t.members.size(),
                        t.enumValues.size(), t.attributes.size());
            for (const auto& m : t.members) {
                std::printf("  %-30s %-25s offset %u size %u%s%s\n", m.name.c_str(), m.typeName.c_str(), m.offset,
                            m.size, m.bitField ? " bits" : "", m.property ? " property" : "");
            }
            for (const auto& e : t.enumValues) std::printf("  %s = %lld\n", e.name.c_str(), (long long)e.value);
        }
        AdsSymbolTable table(ParseSymbols(symbolData.data(), symbolData.size()),
                             ParseDataTypes(typeData.data(), typeData.size()));
        size_t leaves = 0;
        table.ForEachLeaf([&](const AdsElement& e) {
            ++leaves;
            std::printf("leaf %-50s %-28s kind %d 0x%X:0x%X size %u bit %u\n", e.path.c_str(), e.typeName.c_str(),
                        static_cast<int>(e.kind), e.indexGroup, e.indexOffset, e.size, e.bitOffset);
        });
        std::printf("%zu leaves\n", leaves);
    } catch (const AdsException& e) {
        std::printf("FAILED: 0x%lX\n", e.errorCode);
        return 1;
    }
    return 0;
}

} // namespace

// Usage: symbols-fake                                  built data, for CTest
//        symbols-fake <symbols.bin> <datatypes.bin>    an upload saved from a real PLC
int main(int argc, char** argv) {
    if (argc == 3) return Recorded(argv[1], argv[2]);
    Symbols();
    DataTypes();
    CodePage();
    Broken();
    Table();
    Find();
    std::printf("%s, %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
