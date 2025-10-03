// Copyright (c) 2018 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_PORT_PORT_STDCXX_H_
#define STORAGE_LEVELDB_PORT_PORT_STDCXX_H_

// port/port_config.h availability is automatically detected via __has_include
// in newer compilers. If LEVELDB_HAS_PORT_CONFIG_H is defined, it overrides the
// configuration detection.
#if defined(LEVELDB_HAS_PORT_CONFIG_H)

#if LEVELDB_HAS_PORT_CONFIG_H
#include "port/port_config.h"
#endif  // LEVELDB_HAS_PORT_CONFIG_H

#elif defined(__has_include)

#if __has_include("port/port_config.h")
#include "port/port_config.h"
#endif  // __has_include("port/port_config.h")

#endif  // defined(LEVELDB_HAS_PORT_CONFIG_H)

#if HAVE_CRC32C
#include <crc32c/crc32c.h>
#endif  // HAVE_CRC32C
#if HAVE_SNAPPY
#include <snappy.h>
#endif  // HAVE_SNAPPY
#if HAVE_ZSTD
#define ZSTD_STATIC_LINKING_ONLY  // For ZSTD_compressionParameters.
#include <zstd.h>
#endif  // HAVE_ZSTD
#if HAVE_ZLIB
#include <zlib.h>
#endif // HAVE_ZLIB

#include <cassert>
#include <condition_variable>  // NOLINT
#include <cstddef>
#include <cstdint>
#include <mutex>  // NOLINT
#include <string>

#include "port/thread_annotations.h"

namespace leveldb {
namespace port {

class CondVar;

static const int kLogBlockSize = 65536;

// Thinly wraps std::mutex.
class LOCKABLE Mutex {
 public:
  Mutex() = default;
  ~Mutex() = default;

  Mutex(const Mutex&) = delete;
  Mutex& operator=(const Mutex&) = delete;

  void Lock() EXCLUSIVE_LOCK_FUNCTION() { mu_.lock(); }
  void Unlock() UNLOCK_FUNCTION() { mu_.unlock(); }
  void AssertHeld() ASSERT_EXCLUSIVE_LOCK() {}

 private:
  friend class CondVar;
  std::mutex mu_;
};

// Thinly wraps std::condition_variable.
class CondVar {
 public:
  explicit CondVar(Mutex* mu) : mu_(mu) { assert(mu != nullptr); }
  ~CondVar() = default;

  CondVar(const CondVar&) = delete;
  CondVar& operator=(const CondVar&) = delete;

  void Wait() {
    std::unique_lock<std::mutex> lock(mu_->mu_, std::adopt_lock);
    cv_.wait(lock);
    lock.release();
  }
  void Signal() { cv_.notify_one(); }
  void SignalAll() { cv_.notify_all(); }

 private:
  std::condition_variable cv_;
  Mutex* const mu_;
};

inline bool Snappy_Compress(const char* input, size_t length,
                            std::string* output) {
#if HAVE_SNAPPY
  output->resize(snappy::MaxCompressedLength(length));
  size_t outlen;
  snappy::RawCompress(input, length, &(*output)[0], &outlen);
  output->resize(outlen);
  return true;
#else
  // Silence compiler warnings about unused arguments.
  (void)input;
  (void)length;
  (void)output;
#endif  // HAVE_SNAPPY

  return false;
}

inline bool Snappy_GetUncompressedLength(const char* input, size_t length,
                                         size_t* result) {
#if HAVE_SNAPPY
  return snappy::GetUncompressedLength(input, length, result);
#else
  // Silence compiler warnings about unused arguments.
  (void)input;
  (void)length;
  (void)result;
  return false;
#endif  // HAVE_SNAPPY
}

inline bool Snappy_Uncompress(const char* input, size_t length, char* output) {
#if HAVE_SNAPPY
  return snappy::RawUncompress(input, length, output);
#else
  // Silence compiler warnings about unused arguments.
  (void)input;
  (void)length;
  (void)output;
  return false;
#endif  // HAVE_SNAPPY
}

inline bool Zstd_Compress(int level, const char* input, size_t length,
                          std::string* output) {
#if HAVE_ZSTD
  // Get the MaxCompressedLength.
  size_t outlen = ZSTD_compressBound(length);
  if (ZSTD_isError(outlen)) {
    return false;
  }
  output->resize(outlen);
  ZSTD_CCtx* ctx = ZSTD_createCCtx();
  ZSTD_compressionParameters parameters =
      ZSTD_getCParams(level, std::max(length, size_t{1}), /*dictSize=*/0);
  ZSTD_CCtx_setCParams(ctx, parameters);
  outlen = ZSTD_compress2(ctx, &(*output)[0], output->size(), input, length);
  ZSTD_freeCCtx(ctx);
  if (ZSTD_isError(outlen)) {
    return false;
  }
  output->resize(outlen);
  return true;
#else
  // Silence compiler warnings about unused arguments.
  (void)level;
  (void)input;
  (void)length;
  (void)output;
  return false;
#endif  // HAVE_ZSTD
}

inline bool Zstd_GetUncompressedLength(const char* input, size_t length,
                                       size_t* result) {
#if HAVE_ZSTD
  size_t size = ZSTD_getFrameContentSize(input, length);
  if (size == 0) return false;
  *result = size;
  return true;
#else
  // Silence compiler warnings about unused arguments.
  (void)input;
  (void)length;
  (void)result;
  return false;
#endif  // HAVE_ZSTD
}

inline bool Zstd_Uncompress(const char* input, size_t length, char* output) {
#if HAVE_ZSTD
  size_t outlen;
  if (!Zstd_GetUncompressedLength(input, length, &outlen)) {
    return false;
  }
  ZSTD_DCtx* ctx = ZSTD_createDCtx();
  outlen = ZSTD_decompressDCtx(ctx, output, outlen, input, length);
  ZSTD_freeDCtx(ctx);
  if (ZSTD_isError(outlen)) {
    return false;
  }
  return true;
#else
  // Silence compiler warnings about unused arguments.
  (void)input;
  (void)length;
  (void)output;
  return false;
#endif  // HAVE_ZSTD
}

inline bool Zlib_Compress(int level, const char* input, size_t length,
                          std::string* output, uint8_t raw = 0) {
#if HAVE_ZLIB
  const size_t BUFSIZE = 128 * 1024;
  unsigned char temp_buffer[BUFSIZE];
  // reserve enough memory to not reallocate on the fly
  output->reserve(compressBound(length));
  z_stream strm;
  strm.zalloc = 0;
  strm.zfree = 0;
  strm.next_in = (unsigned char*)(input);
  strm.avail_in = (uint32_t)length;
  strm.next_out = temp_buffer;
  strm.avail_out = BUFSIZE;
  int res = deflateInit2(&strm, level, Z_DEFLATED, raw ? -15 : 15, 8,
                          Z_DEFAULT_STRATEGY);
  if (res != Z_OK) {
    return false;
  }
  while (strm.avail_in != 0) {
    res = deflate(&strm, Z_NO_FLUSH);
    if (res != Z_OK) {
      return false;
    }
    if (strm.avail_out == 0) {
      output->append(temp_buffer, temp_buffer + BUFSIZE);
      strm.next_out = temp_buffer;
      strm.avail_out = BUFSIZE;
    }
  }
  while (res == Z_OK) {
    if (strm.avail_out == 0) {
      output->append(temp_buffer, temp_buffer + BUFSIZE);
      strm.next_out = temp_buffer;
      strm.avail_out = BUFSIZE;
    }
    res = deflate(&strm, Z_FINISH);
  }
  if (res != Z_STREAM_END) {
    return false;
  }
  output->append(temp_buffer, temp_buffer + BUFSIZE - strm.avail_out);
  (void)deflateEnd(&strm);
  return true;
#else
  // Silence compiler warnings about unused arguments.
  (void)level;
  (void)input;
  (void)length;
  (void)output;
  (void)raw;
  return false;
#endif  // HAVE_ZLIB
}

inline bool Zlib_Uncompress(const char* input, size_t length,
                            std::string* output, uint8_t raw = 0)
{
#if HAVE_ZLIB
  const int CHUNK = 64 * 1024;
  int ret;
  size_t have;
  z_stream strm;
  unsigned char out[CHUNK];
  /* allocate inflate state */
  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;
  strm.avail_in = (uint32_t)length;
  strm.next_in = (Bytef*)input;
  ret = inflateInit2(&strm, (raw ? -15 : 15));
  if (ret != Z_OK) {
    return false;
  }
  /* run inflate() on input until output buffer not full */
  do {
    strm.avail_out = CHUNK;
    strm.next_out = out;
    ret = ::inflate(&strm, Z_NO_FLUSH);
    if (ret == Z_NEED_DICT) {
      ret = Z_DATA_ERROR;
    }
    if (ret < 0) {
      (void)inflateEnd(&strm);
      return false;
    }
    have = CHUNK - strm.avail_out;
    output->append((char*)out, have);
  } while (strm.avail_out == 0);
  /* clean up and return */
  (void)inflateEnd(&strm);
  return ret == Z_STREAM_END;
#else
  // Silence compiler warnings about unused arguments.
  (void)input;
  (void)length;
  (void)output;
  (void)raw;
  return false;
#endif  // HAVE_ZLIB
}

inline bool GetHeapProfile(void (*func)(void*, const char*, int), void* arg) {
  // Silence compiler warnings about unused arguments.
  (void)func;
  (void)arg;
  return false;
}

inline uint32_t AcceleratedCRC32C(uint32_t crc, const char* buf, size_t size) {
#if HAVE_CRC32C
  return ::crc32c::Extend(crc, reinterpret_cast<const uint8_t*>(buf), size);
#else
  // Silence compiler warnings about unused arguments.
  (void)crc;
  (void)buf;
  (void)size;
  return 0;
#endif  // HAVE_CRC32C
}

}  // namespace port
}  // namespace leveldb

#endif  // STORAGE_LEVELDB_PORT_PORT_STDCXX_H_
