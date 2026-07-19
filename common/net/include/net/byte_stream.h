#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace net {

// Little-endian wire encoding. Every read is bounds-checked and sets a failure
// flag rather than throwing or reading out of bounds: packet data is untrusted
// input, so a malformed message must never be able to crash a server.

class ByteWriter {
public:
    explicit ByteWriter(std::vector<uint8_t>& buffer) : mBuffer(buffer) {}

    void u8(uint8_t v) { raw(&v, 1); }
    void u16(uint16_t v) { little(v); }
    void u32(uint32_t v) { little(v); }
    void u64(uint64_t v) { little(v); }
    void i32(int32_t v) { little(static_cast<uint32_t>(v)); }

    void f32(float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, sizeof bits);
        little(bits);
    }

    // Strings are length-prefixed and capped, so a hostile peer can't ask us to
    // allocate an enormous buffer.
    void string(const std::string& v) {
        const uint16_t length = static_cast<uint16_t>(
                v.size() > kMaxStringLength ? kMaxStringLength : v.size());
        u16(length);
        raw(reinterpret_cast<const uint8_t*>(v.data()), length);
    }

    void raw(const uint8_t* data, size_t size) {
        mBuffer.insert(mBuffer.end(), data, data + size);
    }

    static constexpr size_t kMaxStringLength = 1024;

private:
    template <typename T>
    void little(T v) {
        uint8_t bytes[sizeof(T)];
        for (size_t i = 0; i < sizeof(T); ++i) {
            bytes[i] = static_cast<uint8_t>(v >> (8 * i));
        }
        raw(bytes, sizeof(T));
    }

    std::vector<uint8_t>& mBuffer;
};

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size) : mData(data), mSize(size) {}

    uint8_t u8() { return take<uint8_t>(); }
    uint16_t u16() { return take<uint16_t>(); }
    uint32_t u32() { return take<uint32_t>(); }
    uint64_t u64() { return take<uint64_t>(); }
    int32_t i32() { return static_cast<int32_t>(take<uint32_t>()); }

    float f32() {
        const uint32_t bits = take<uint32_t>();
        float v;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }

    std::string string() {
        const uint16_t length = u16();
        if (mFailed || length > ByteWriter::kMaxStringLength || remaining() < length) {
            mFailed = true;
            return {};
        }
        std::string out(reinterpret_cast<const char*>(mData + mOffset), length);
        mOffset += length;
        return out;
    }

    size_t remaining() const { return mFailed ? 0 : mSize - mOffset; }

    // True if any read ran past the end of the buffer. Callers must check this
    // before acting on decoded values.
    bool failed() const { return mFailed; }

private:
    template <typename T>
    T take() {
        if (remaining() < sizeof(T)) {
            mFailed = true;
            return T{};
        }
        T v{};
        for (size_t i = 0; i < sizeof(T); ++i) {
            v |= static_cast<T>(static_cast<T>(mData[mOffset + i]) << (8 * i));
        }
        mOffset += sizeof(T);
        return v;
    }

    const uint8_t* mData;
    size_t mSize;
    size_t mOffset = 0;
    bool mFailed = false;
};

}  // namespace net
