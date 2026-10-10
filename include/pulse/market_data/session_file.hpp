#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "pulse/core/instrument.hpp"

// Session file (.pulse) — the unit of record and replay.
//
//   FileHeader                      fixed 64 bytes
//   InstrumentRecord x count        48 bytes each
//   Record x N                      RecordHeader (16 bytes) + raw frame bytes
//
// Records wrap raw wire frames *as received*, including duplicates, corrupt
// frames and retransmissions. The record header stores the byte length, so a
// frame whose own length field is corrupt never desynchronises the reader.
// Replaying a recording therefore reproduces the exact input that the feed
// handler saw — faults included — which is what makes replay useful for
// debugging.

namespace pulse::md {

inline constexpr char kSessionMagic[8] = {'P', 'U', 'L', 'S', 'E', 'S', 'S', '1'};
inline constexpr std::uint32_t kSessionVersion = 1;

struct SessionInfo {
  InstrumentUniverse universe;
  std::uint64_t seed = 0;
  std::uint64_t record_count = 0;  // 0 if the writer did not know (streamed)
  std::uint64_t start_ts = 0;
};

struct Record {
  std::span<const std::byte> frame;
  std::uint64_t capture_ns = 0;  // when the frame was captured (0 = synthetic)
};

class SessionWriter {
 public:
  SessionWriter(const std::string& path, const SessionInfo& info);
  ~SessionWriter();
  SessionWriter(const SessionWriter&) = delete;
  SessionWriter& operator=(const SessionWriter&) = delete;

  void write(std::span<const std::byte> frame, std::uint64_t capture_ns = 0);
  // Patches record_count in the header and closes the file.
  void close();
  std::uint64_t records() const noexcept { return records_; }

 private:
  std::FILE* f_ = nullptr;
  std::vector<char> buf_;
  std::uint64_t records_ = 0;
};

// Iterates records over a byte range: either a memory-mapped file or an
// in-memory buffer (benchmarks generate sessions straight into memory).
class SessionReader {
 public:
  static SessionReader open(const std::string& path);                   // mmap
  static SessionReader from_memory(std::vector<std::byte> bytes);      // owns

  SessionReader(SessionReader&&) noexcept;
  SessionReader& operator=(SessionReader&&) noexcept;
  ~SessionReader();

  const SessionInfo& info() const noexcept { return info_; }
  bool next(Record& out) noexcept;
  void rewind() noexcept { pos_ = data_start_; }
  std::size_t bytes() const noexcept { return size_; }

 private:
  SessionReader() = default;
  void parse_header();

  const std::byte* base_ = nullptr;
  std::size_t size_ = 0;
  std::size_t data_start_ = 0;
  std::size_t pos_ = 0;
  bool mapped_ = false;
  std::vector<std::byte> owned_;
  SessionInfo info_;
};

// Builds a session in memory with the same layout SessionWriter produces.
class MemorySessionBuilder {
 public:
  explicit MemorySessionBuilder(const SessionInfo& info);
  void write(std::span<const std::byte> frame, std::uint64_t capture_ns = 0);
  std::vector<std::byte> finish() &&;

 private:
  std::vector<std::byte> bytes_;
  std::uint64_t records_ = 0;
};

}  // namespace pulse::md
