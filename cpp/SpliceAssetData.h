/**
 * @copyright Copyright (c) 2024 Splice, Inc. All rights reserved.
 */

#pragma once
#include <cassert>
#include <string>
#include <cstddef>
#include <vector>

constexpr auto splice_header = "sp";
constexpr auto section_size = 8;
constexpr auto key_size = 18;
constexpr auto encoding_header_size =
    std::char_traits<char>::length(splice_header) + section_size + key_size;

namespace splice
{
class SpliceAssetData
{
public:
    /// Check if data is scrambled
    static bool isScrambled(const char* inData, size_t sizeInBytes)
    {
        // determine if the data is encoded by looking for the "sp" header
        return (sizeInBytes >= encoding_header_size)
               && memcmp(inData, splice_header, strlen(splice_header)) == 0;
    }

    static void readAndDecodeSection(
        char* destination, const char* source, const char* key, uint64_t size)
    {
        char byte;
        auto keyIndex = 0;
        for (uint64_t i = 0; i < size; i++)
        {
            // read a byte at a time
            byte = source[i];
    
            // xor with the current index of the key
            destination[i] = byte ^ key[keyIndex];
    
            // increment the key
            keyIndex = (keyIndex + 1) % key_size;
        }
    }

    /// Descrambles audio data if necessary and stores it in the returned buffer.
    /// If audio data is not scrambled it will be copied into the returned buffer.
    /// It is up to the caller to deallocate the buffer.
    static std::vector<char> descrambleAudioData(const char* data, size_t sizeInBytes)
    {
        assert(data != nullptr);
        if (!data)
        {
            return {};
        }
        // allocate the internal buffer
        const bool encoded = isScrambled(data, sizeInBytes);
        const auto totalSize = sizeInBytes - (encoded ? encoding_header_size : 0);
        auto buffer = std::vector<char>(totalSize);
        if (encoded)
        {
            auto index = strlen(splice_header);
    
            // section size, should be quarter of the file
            // little endian uint64_t
            uint64_t size = 0;
            for (size_t i = 0; i < section_size; i++)
            {
                size += ((uint64_t) (0xFF & data[index + i]) << (8 * i));
            }
            index += section_size;
    
            // size check
            assert((size * 4) <= totalSize);
            if ((size * 4) > totalSize)
            {
                return {};
            }
    
            // key
            char key[key_size];
            memcpy(key, data + index, key_size);
            index += key_size;
    
            auto* dest = buffer.data();
    
            // decode with key 1/4
            readAndDecodeSection(dest, data + index, key, size);
            dest += size;
            index += size;
    
            // read 1/4 as is
            memcpy(dest, data + index, size);
            dest += size;
            index += size;
    
            // decode with key 1/4
            readAndDecodeSection(dest, data + index, key, size);
            dest += size;
            index += size;
    
            // read the rest
            memcpy(dest, data + index, size + (totalSize % 4));
        }
        else
        {
            memcpy(buffer.data(), data, sizeInBytes);
        }
        return buffer;
    }
};

} // namespace splice
