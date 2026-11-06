// Builds symbol and data type uploads in the binary format of TwinCAT (ADSIGRP_SYM_UPLOAD,
// ADSIGRP_SYM_DT_UPLOAD), so that the tests need neither TwinCAT nor recorded data. Entries carry a type GUID and
// are padded with zeros to a multiple of 8 bytes, as TwinCAT does.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace upload {

using Bytes = std::vector<uint8_t>;
using Attributes = std::vector<std::pair<std::string, std::string>>;

inline void Put(Bytes& b, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

inline void PutText(Bytes& b, const std::string& s) {
    b.insert(b.end(), s.begin(), s.end());
    b.push_back(0);
}

inline void PutAttributes(Bytes& b, const Attributes& attributes) {
    Put(b, attributes.size(), 2);
    for (const auto& a : attributes) {
        Put(b, a.first.size(), 1);
        Put(b, a.second.size(), 1);
        PutText(b, a.first);
        PutText(b, a.second);
    }
}

inline void PutGuid(Bytes& b) {
    for (int i = 0; i < 16; ++i) b.push_back(static_cast<uint8_t>(0xA0 + i));
}

// Sets the length at the start of an entry after padding it.
inline void Finish(Bytes& entry) {
    while (entry.size() % 8) entry.push_back(0);
    for (int i = 0; i < 4; ++i) entry[i] = static_cast<uint8_t>(entry.size() >> (8 * i));
}

struct Symbol {
    std::string name, type, comment;
    uint32_t group = 0x4040, offset = 0, size = 0, dataType = 65;
    uint32_t flags = 0x8; // TYPEGUID, as every symbol of TwinCAT has
    Attributes attributes; // sets flag 0x1000
    bool extendedFlags = false; // sets flag 0x8000

    Bytes Build() const {
        uint32_t f = flags | (attributes.empty() ? 0 : 0x1000) | (extendedFlags ? 0x8000 : 0);
        Bytes b;
        Put(b, 0, 4);
        Put(b, group, 4);
        Put(b, offset, 4);
        Put(b, size, 4);
        Put(b, dataType, 4);
        Put(b, f, 4);
        Put(b, name.size(), 2);
        Put(b, type.size(), 2);
        Put(b, comment.size(), 2);
        PutText(b, name);
        PutText(b, type);
        PutText(b, comment);
        if (f & 0x8) PutGuid(b);
        if (f & 0x1000) PutAttributes(b, attributes);
        if (f & 0x8000) Put(b, 0x9, 4);
        Finish(b);
        return b;
    }
};

struct Type {
    std::string name, type, comment;
    uint32_t size = 0, offset = 0, dataType = 65;
    uint32_t flags = 0x80; // TYPEGUID; 0x1 data type, 0x2 data item are added by Build
    std::vector<std::pair<int32_t, uint32_t>> dims;
    std::vector<Type> members;
    Attributes attributes; // sets flag 0x1000
    std::vector<std::pair<std::string, int64_t>> enumValues; // sets flag 0x2000
    unsigned methods = 0; // sets flag 0x800
    Bytes copyMask; // sets flag 0x200, must have size bytes
    Bytes unknownTail; // after the known parts, as a newer TwinCAT might send

    Bytes Build(bool member = false) const {
        uint32_t f = flags | (member ? 0x2 : 0x1) | (attributes.empty() ? 0 : 0x1000) |
                     (enumValues.empty() ? 0 : 0x2000) | (methods ? 0x800 : 0) | (copyMask.empty() ? 0 : 0x200);
        Bytes b;
        Put(b, 0, 4);
        Put(b, 1, 4); // version
        Put(b, 0x11111111, 4);
        Put(b, 0x22222222, 4);
        Put(b, size, 4);
        Put(b, offset, 4);
        Put(b, dataType, 4);
        Put(b, f, 4);
        Put(b, name.size(), 2);
        Put(b, type.size(), 2);
        Put(b, comment.size(), 2);
        Put(b, dims.size(), 2);
        Put(b, members.size(), 2);
        PutText(b, name);
        PutText(b, type);
        PutText(b, comment);
        for (const auto& d : dims) {
            Put(b, static_cast<uint32_t>(d.first), 4);
            Put(b, d.second, 4);
        }
        for (const auto& m : members) {
            const auto e = m.Build(true);
            b.insert(b.end(), e.begin(), e.end());
        }
        if (f & 0x80) PutGuid(b);
        if (f & 0x200) b.insert(b.end(), copyMask.begin(), copyMask.end());
        if (f & 0x800) {
            Put(b, methods, 2);
            for (unsigned i = 0; i < methods; ++i) {
                Bytes m(4); // a method entry: its length, then content the parser skips
                PutText(m, "Method" + std::to_string(i));
                Finish(m);
                b.insert(b.end(), m.begin(), m.end());
            }
        }
        if (f & 0x1000) PutAttributes(b, attributes);
        if (f & 0x2000) {
            Put(b, enumValues.size(), 2);
            for (const auto& e : enumValues) {
                Put(b, e.first.size(), 1);
                PutText(b, e.first);
                Put(b, static_cast<uint64_t>(e.second), size);
            }
        }
        b.insert(b.end(), unknownTail.begin(), unknownTail.end());
        Finish(b);
        return b;
    }
};

template <typename T> Bytes Join(const std::vector<T>& entries) {
    Bytes all;
    for (const auto& e : entries) {
        const auto b = e.Build();
        all.insert(all.end(), b.begin(), b.end());
    }
    return all;
}

} // namespace upload
