#include "pulse/market_data/session_file.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>

namespace pulse::md {
namespace {

// On-disk layouts. Written with memcpy field by field so struct padding never
// leaks into the file format.
constexpr std::size_t kFileHeaderSize = 64;
constexpr std::size_t kInstrumentRecordSize = 48;
constexpr std::size_t kRecordHeaderSize = 16;
constexpr std::size_t kRecordCountOffset = 24;

template <class T>
void put(std::byte* p, std::size_t off, T v) {
  std::memcpy(p + off, &v, sizeof(T));
}
template <class T>
T get(const std::byte* p, std::size_t off) {
  T v;
  std::memcpy(&v, p + off, sizeof(T));
  return v;
}

std::vector<std::byte> encode_header(const SessionInfo& info) {
  const auto n = info.universe.size();
  std::vector<std::byte> out(kFileHeaderSize + n * kInstrumentRecordSize);
  std::byte* p = out.data();
  std::memcpy(p, kSessionMagic, 8);
  put<std::uint32_t>(p, 8, kSessionVersion);
  put<std::uint32_t>(p, 12, static_cast<std::uint32_t>(n));
  put<std::uint64_t>(p, 16, info.seed);
  put<std::uint64_t>(p, kRecordCountOffset, info.record_count);
  put<std::uint64_t>(p, 32, info.start_ts);
  for (std::size_t i = 0; i < n; ++i) {
    const auto& s = info.universe[static_cast<InstrumentId>(i)];
    std::byte* r = p + kFileHeaderSize + i * kInstrumentRecordSize;
    put<std::uint32_t>(r, 0, s.id);
    std::memcpy(r + 4, s.symbol.data(), 8);
    put<std::int64_t>(r, 16, s.tick_size);
    put<std::int64_t>(r, 24, s.min_price);
    put<std::int64_t>(r, 32, s.max_price);
    put<std::int64_t>(r, 40, s.reference_price);
  }
  return out;
}

void append_record(std::vector<std::byte>& out, std::span<const std::byte> frame, std::uint64_t capture_ns) {
  std::byte hdr[kRecordHeaderSize]{};
  put<std::uint32_t>(hdr, 0, static_cast<std::uint32_t>(frame.size()));
  put<std::uint64_t>(hdr, 8, capture_ns);
  out.insert(out.end(), hdr, hdr + kRecordHeaderSize);
  out.insert(out.end(), frame.begin(), frame.end());
}

}  // namespace

// --------------------------------------------------------------------------
SessionWriter::SessionWriter(const std::string& path, const SessionInfo& info) : buf_(1 << 20) {
  f_ = std::fopen(path.c_str(), "wb");
  if (!f_) throw std::runtime_error("cannot open for writing: " + path);
  std::setvbuf(f_, buf_.data(), _IOFBF, buf_.size());
  const auto hdr = encode_header(info);
  std::fwrite(hdr.data(), 1, hdr.size(), f_);
}

SessionWriter::~SessionWriter() {
  if (f_) close();
}

void SessionWriter::write(std::span<const std::byte> frame, std::uint64_t capture_ns) {
  std::byte hdr[kRecordHeaderSize]{};
  put<std::uint32_t>(hdr, 0, static_cast<std::uint32_t>(frame.size()));
  put<std::uint64_t>(hdr, 8, capture_ns);
  std::fwrite(hdr, 1, kRecordHeaderSize, f_);
  std::fwrite(frame.data(), 1, frame.size(), f_);
  ++records_;
}

void SessionWriter::close() {
  if (!f_) return;
  std::fflush(f_);
  std::fseek(f_, static_cast<long>(kRecordCountOffset), SEEK_SET);
  std::fwrite(&records_, sizeof(records_), 1, f_);
  std::fclose(f_);
  f_ = nullptr;
}

// --------------------------------------------------------------------------
SessionReader SessionReader::open(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) throw std::runtime_error("cannot open session: " + path);
  struct stat st {};
  if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(kFileHeaderSize)) {
    ::close(fd);
    throw std::runtime_error("not a session file (too small): " + path);
  }
  const auto size = static_cast<std::size_t>(st.st_size);
  void* m = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (m == MAP_FAILED) throw std::runtime_error("mmap failed: " + path);
  ::madvise(m, size, MADV_SEQUENTIAL);
  SessionReader r;
  r.base_ = static_cast<const std::byte*>(m);
  r.size_ = size;
  r.mapped_ = true;
  r.parse_header();
  return r;
}

SessionReader SessionReader::from_memory(std::vector<std::byte> bytes) {
  SessionReader r;
  r.owned_ = std::move(bytes);
  r.base_ = r.owned_.data();
  r.size_ = r.owned_.size();
  r.parse_header();
  return r;
}

SessionReader::SessionReader(SessionReader&& o) noexcept { *this = std::move(o); }

SessionReader& SessionReader::operator=(SessionReader&& o) noexcept {
  if (this == &o) return *this;
  if (mapped_ && base_) ::munmap(const_cast<std::byte*>(base_), size_);
  owned_ = std::move(o.owned_);
  mapped_ = o.mapped_;
  base_ = mapped_ ? o.base_ : owned_.data();
  size_ = o.size_;
  data_start_ = o.data_start_;
  pos_ = o.pos_;
  info_ = std::move(o.info_);
  o.base_ = nullptr;
  o.mapped_ = false;
  o.size_ = 0;
  return *this;
}

SessionReader::~SessionReader() {
  if (mapped_ && base_) ::munmap(const_cast<std::byte*>(base_), size_);
}

void SessionReader::parse_header() {
  if (size_ < kFileHeaderSize || std::memcmp(base_, kSessionMagic, 8) != 0) {
    throw std::runtime_error("bad session magic");
  }
  if (get<std::uint32_t>(base_, 8) != kSessionVersion) throw std::runtime_error("unsupported session version");
  const auto n = get<std::uint32_t>(base_, 12);
  info_.seed = get<std::uint64_t>(base_, 16);
  info_.record_count = get<std::uint64_t>(base_, kRecordCountOffset);
  info_.start_ts = get<std::uint64_t>(base_, 32);
  if (size_ < kFileHeaderSize + n * kInstrumentRecordSize) throw std::runtime_error("truncated session header");
  std::vector<InstrumentSpec> specs;
  specs.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const std::byte* r = base_ + kFileHeaderSize + i * kInstrumentRecordSize;
    InstrumentSpec s;
    s.id = get<std::uint32_t>(r, 0);
    std::memcpy(s.symbol.data(), r + 4, 8);
    s.tick_size = get<std::int64_t>(r, 16);
    s.min_price = get<std::int64_t>(r, 24);
    s.max_price = get<std::int64_t>(r, 32);
    s.reference_price = get<std::int64_t>(r, 40);
    specs.push_back(s);
  }
  info_.universe = InstrumentUniverse(std::move(specs));
  data_start_ = pos_ = kFileHeaderSize + n * kInstrumentRecordSize;
}

bool SessionReader::next(Record& out) noexcept {
  if (pos_ + kRecordHeaderSize > size_) return false;
  const auto len = get<std::uint32_t>(base_, pos_);
  const auto cap = get<std::uint64_t>(base_, pos_ + 8);
  if (pos_ + kRecordHeaderSize + len > size_) return false;  // torn tail
  out.frame = {base_ + pos_ + kRecordHeaderSize, len};
  out.capture_ns = cap;
  pos_ += kRecordHeaderSize + len;
  return true;
}

// --------------------------------------------------------------------------
MemorySessionBuilder::MemorySessionBuilder(const SessionInfo& info) : bytes_(encode_header(info)) {}

void MemorySessionBuilder::write(std::span<const std::byte> frame, std::uint64_t capture_ns) {
  append_record(bytes_, frame, capture_ns);
  ++records_;
}

std::vector<std::byte> MemorySessionBuilder::finish() && {
  put<std::uint64_t>(bytes_.data(), kRecordCountOffset, records_);
  return std::move(bytes_);
}

}  // namespace pulse::md
