//
// Copyright 2017 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#include "pxr/pxr.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/fastCompression.h"
#include "pxr/usd/usd/integerCoding.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>

PXR_NAMESPACE_OPEN_SCOPE

/*

These integer coding & compression routines are tailored for what are typically
lists of indexes into other tables.  The binary "usdc" file format has lots of
these in its "structural sections" that define the object hierarchy.

The basic idea is to take a contiguous list of 32-bit integers and encode them
in a buffer that is not only smaller, but also still quite compressible by a
general compression algorithm, and then compress that buffer to produce a final
result.  Decompression proceeds by going in reverse.  The general compressor is
LZ4 via TfFastCompression.  The integer coding scheme implemented here is
described below.

We encode a list of integers as follows.  First we transform the input to
produce a new list of integers where each element is the difference between it
and the previous integer in the input sequence.  This is the sequence we encode.
Next we find the most common value in the sequence and write it to the output.
Then we write 2-bit codes, one for each integer, classifying it.  Finally we
write a variable length section of integer data.  The decoder uses the 2-bit
codes to understand how to interpret this variable length data.

Given a list of integers, say:

input = [123, 124, 125, 100125, 100125, 100126, 10026]

we encode as follows.  First, we transform the list to be the list of
differences to the previous integer, or the integer itself for the first element
in the list (this can be considered a difference to 0) to get:

input_diffs = [123, 1, 1, 100000, 0, 1, 0]

Then we find the most commonly occurring value in this sequence, which is '1'.
We write this most commonly occurring value into the output stream.

output = [int32(1)]

Next we write two sections, first a fixed length section, 2-bit codes per
integer, followed by a variable length section of integer data.  The two bit
code indicates what "kind" of integer we have:

00: The most common value
01:  8-bit integer
10: 16-bit integer
11: 32-bit integer

For our example this gives:

input  = [123, 124, 125, 100125, 100125, 100126, 10026]
output = [int32(1) 01 00 00 11 01 00 01 XX int8(123) int32(100000) int8(0) int8(0)]

Where 'XX' represents unused bits in the last byte of the codes section to round
up to an even number of bytes.

In this case the output size is 12 bytes compared to the original input which
was 28 bytes.  In the best possible case the output is (asymptotically) 2 bits
per integer (6.25% the original size), in the worst possible case it is
(asymptotically) 34 bits per integer (106.25% the original size).

*/

namespace {

inline int32_t
_Signed(uint32_t x)
{
    if (x <= static_cast<uint32_t>(INT32_MAX))
        return static_cast<int32_t>(x);

    if (x >= static_cast<uint32_t>(INT32_MIN))
        return static_cast<int32_t>(x - INT32_MIN) + INT32_MIN;

    TF_FATAL_ERROR("Unsupported C++ integer representation");
    return 0;
}

template <class T>
inline char *_WriteBits(char *p, T val)
{
    memcpy(p, &val, sizeof(val));
    return p + sizeof(val);
}

template <class T>
inline T _ReadBits(char const *&p)
{
    T ret;
    memcpy(&ret, p, sizeof(ret));
    p += sizeof(ret);
    return ret;
}

template <class Int>
constexpr size_t
_GetEncodedBufferSize(size_t numInts)
{
    // Calculate encoded integer size.
    return numInts ?
        /* commonValue */ (sizeof(Int)) +
        /* numCodesBytes */ ((numInts * 2 + 7) / 8) +
        /* maxIntBytes */ (numInts * sizeof(Int))
        : 0;
}

template <int N, class Iterator>
void _EncodeNHelper(
    Iterator &cur,
    typename std::iterator_traits<Iterator>::value_type commonValue,
    typename std::make_signed<
        typename std::iterator_traits<Iterator>::value_type
    >::type &prevVal,
    char *&codesOut,
    char *&vintsOut) {

    using Int = typename std::iterator_traits<Iterator>::value_type;
    using SInt = typename std::make_signed<Int>::type;

    static_assert(1 <= N && N <= 4, "");

    enum Code { Common, One, Two, Four };
    
    auto getCode = [commonValue](SInt x) {
        std::numeric_limits<int8_t> int8Limit;
        std::numeric_limits<int16_t> int16Limit;
        if (x == commonValue) { return Common; }
        if (x >= int8Limit.min() && x <= int8Limit.max()) { return One; }
        if (x >= int16Limit.min() && x <= int16Limit.max()) { return Two; }
        return Four;
    };

    uint8_t codeByte = 0;
    for (int i = 0; i != N; ++i) {
        SInt val = _Signed(*cur) - prevVal;
        prevVal = _Signed(*cur++);
        Code code = getCode(val);
        codeByte |= (code << (2 * i));
        switch (code) {
        default:
        case Common:
            break;
        case One:
            vintsOut = _WriteBits(vintsOut, static_cast<int8_t>(val));
            break;
        case Two:
            vintsOut = _WriteBits(vintsOut, static_cast<int16_t>(val));
            break;
        case Four:
            vintsOut = _WriteBits(vintsOut, static_cast<int32_t>(val));
            break;
        };
    }
    codesOut = _WriteBits(codesOut, codeByte);
}

template <int N, class Iterator>
void _DecodeNHelper(
    char const *&codesIn,
    char const *&vintsIn,
    typename std::iterator_traits<Iterator>::value_type commonValue,
    typename std::make_signed<
        typename std::iterator_traits<Iterator>::value_type
    >::type &prevVal,
    Iterator &output)
{
    using Int = typename std::iterator_traits<Iterator>::value_type;

    enum Code { Common, One, Two, Four };

    uint8_t codeByte = *codesIn++;
    for (int i = 0; i != N; ++i) {
        switch ((codeByte & (3 << (2 * i))) >> (2 * i)) {
        default:
        case Common:
            prevVal += commonValue;
            break;
        case One:
            prevVal += _ReadBits<int8_t>(vintsIn);
            break;
        case Two:
            prevVal += _ReadBits<int16_t>(vintsIn);
            break;
        case Four:
            prevVal += _ReadBits<int32_t>(vintsIn);
            break;
        }
        *output++ = static_cast<Int>(prevVal);
    }
}

template <class Int>
size_t
_EncodeIntegers(Int const *begin, size_t numInts, char *output)
{
    static_assert(sizeof(Int) == sizeof(int32_t), "");

    using SInt = typename std::make_signed<Int>::type;
    
    if (numInts == 0)
        return 0;

    // First find the most common element.
    std::unordered_map<SInt, size_t> counts;
    SInt prevVal = 0;
    for (Int const *cur = begin, *end = begin + numInts;
         cur != end; ++cur) {
        SInt val = _Signed(*cur) - prevVal;
        ++counts[val];
        prevVal = _Signed(*cur);
    }
    SInt commonValue=0;
    {
        size_t commonCount=0;
        for (auto const &p: counts) {
            if (p.second > commonCount) {
                commonCount = p.second;
                commonValue = p.first;
            }
        }
    }

    // Now code the values.
    
    // Write most common value.
    char *p = _WriteBits(output, commonValue);
    char *codesOut = p;
    char *vintsOut = p + (numInts * 2 + 7) / 8;

    Int const *cur = begin;
    prevVal = 0;
    while (numInts >= 4) {
        _EncodeNHelper<4>(cur, commonValue, prevVal, codesOut, vintsOut);
        numInts -= 4;
    }
    switch (numInts) {
    case 0: default: break;
    case 1: _EncodeNHelper<1>(cur, commonValue, prevVal, codesOut, vintsOut);
        break;
    case 2: _EncodeNHelper<2>(cur, commonValue, prevVal, codesOut, vintsOut);
        break;
    case 3: _EncodeNHelper<3>(cur, commonValue, prevVal, codesOut, vintsOut);
        break;
    };
    
    return vintsOut - output;
}

template <class Int>
size_t _DecodeIntegers(char const *data, size_t numInts, Int *result)
{
    static_assert(sizeof(Int) == sizeof(int32_t), "");

    using SInt = typename std::make_signed<Int>::type;

    auto commonValue = _ReadBits<SInt>(data);

    size_t numCodesBytes = (numInts * 2 + 7) / 8;
    char const *codesIn = data;
    char const *vintsIn = data + numCodesBytes;

    SInt prevVal = 0;
    auto intsLeft = numInts;
    while (intsLeft >= 4) {
        _DecodeNHelper<4>(codesIn, vintsIn, commonValue, prevVal, result);
        intsLeft -= 4;
    }
    switch (intsLeft) {
    case 0: default: break;
    case 1: _DecodeNHelper<1>(codesIn, vintsIn, commonValue, prevVal, result);
        break;
    case 2: _DecodeNHelper<2>(codesIn, vintsIn, commonValue, prevVal, result);
        break;
    case 3: _DecodeNHelper<3>(codesIn, vintsIn, commonValue, prevVal, result);
        break;
    };

    return numInts;
}

template <class Int>
size_t
_CompressIntegers(Int const *begin, size_t numInts, char *output)
{
    static_assert(sizeof(Int) == sizeof(int32_t), "");

    // Working space.
    std::unique_ptr<char[]>
        encodeBuffer(new char[_GetEncodedBufferSize<Int>(numInts)]);
    
    // Encode first.
    size_t encodedSize = _EncodeIntegers(begin, numInts, encodeBuffer.get());

    // Then compress.
    return TfFastCompression::CompressToBuffer(
        encodeBuffer.get(), output, encodedSize);
}

template <class Int>
size_t _DecompressIntegers(char const *compressed, size_t compressedSize,
                           Int *ints, size_t numInts, char *workingSpace)
{
    static_assert(sizeof(Int) == sizeof(int32_t), "");

    // Working space.
    size_t workingSpaceSize =
        Usd_IntegerCompression::GetDecompressionWorkingSpaceSize(numInts);
    std::unique_ptr<char[]> tmpSpace;
    if (!workingSpace) {
        tmpSpace.reset(new char[workingSpaceSize]);
        workingSpace = tmpSpace.get();
    }

    size_t decompSz = TfFastCompression::DecompressFromBuffer(
        compressed, workingSpace, compressedSize, workingSpaceSize);

    if (decompSz == 0)
        return 0;

    return _DecodeIntegers(workingSpace, numInts, ints);
}


} // anon

size_t
Usd_IntegerCompression::GetCompressedBufferSize(size_t numInts)
{
    return TfFastCompression::GetCompressedBufferSize(
        _GetEncodedBufferSize<int32_t>(numInts));
}

size_t
Usd_IntegerCompression::GetDecompressionWorkingSpaceSize(size_t numInts)
{
    return _GetEncodedBufferSize<int32_t>(numInts);
}

size_t
Usd_IntegerCompression::CompressToBuffer(
    int32_t const *ints, size_t numInts, char *compressed)
{
    return _CompressIntegers(ints, numInts, compressed);
}

size_t
Usd_IntegerCompression::CompressToBuffer(
    uint32_t const *ints, size_t numInts, char *compressed)
{
    return _CompressIntegers(ints, numInts, compressed);
}

size_t
Usd_IntegerCompression::DecompressFromBuffer(
    char const *compressed, size_t compressedSize,
    int32_t *ints, size_t numInts, char *workingSpace)
{
    return _DecompressIntegers(compressed, compressedSize,
                               ints, numInts, workingSpace);
}

size_t
Usd_IntegerCompression::DecompressFromBuffer(
    char const *compressed, size_t compressedSize,
    uint32_t *ints, size_t numInts, char *workingSpace)
{
    return _DecompressIntegers(compressed, compressedSize,
                               ints, numInts, workingSpace);
}

PXR_NAMESPACE_CLOSE_SCOPE

