// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Session/Keyframe.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <chrono>
#include <mutex>
#include <string_view>
#include <thread>

#include <atomic>

#include <curl/curl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>
#include <fmt/format.h>
#include <picojson.h>
#include <xxh3.h>
#include <zstd.h>

#include "Common/CommonFuncs.h"
#include "Common/CurlTLS.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Core/Orca/Profile.h"

namespace Orca::Net
{
namespace
{
// ORP1 contains controller inputs and typed UI events; legacy machine images are rejected.
constexpr char MAGIC[4] = {'O', 'R', 'P', '1'};
constexpr int WINDOW_LOG = 26;
constexpr int LEVEL = 3;
// Sanity limit per section (MEM2 is 64 MB; a Wii NAND is at most 512 MB).
constexpr u64 MAX_SECTION = 512ull << 20;
constexpr u64 MAX_RAW = 64ull << 20;
// The site accepts at most 128 MB.
constexpr u64 MAX_KEYFRAME_BYTES = 128ull << 20;

class Writer
{
public:
  void U32(u32 v) { Bytes(&v, sizeof(v)); }
  void U64(u64 v) { Bytes(&v, sizeof(v)); }
  void Bytes(const void* data, size_t size)
  {
    if (size > MAX_RAW - out.size())
      throw std::length_error("Replay too large");
    if (size == 0)
      return;
    const auto* p = static_cast<const u8*>(data);
    out.insert(out.end(), p, p + size);
  }
  void Blob(const std::vector<u8>& data)
  {
    U64(data.size());
    Bytes(data.data(), data.size());
  }
  std::vector<u8> out;
};

class Reader
{
public:
  explicit Reader(const std::vector<u8>& data) : m_data(data) {}
  bool Bytes(void* out, size_t size)
  {
    if (size > m_data.size() - m_pos)
      return false;
    std::memcpy(out, m_data.data() + m_pos, size);
    m_pos += size;
    return true;
  }
  bool U32(u32* v) { return Bytes(v, sizeof(*v)); }
  bool U64(u64* v) { return Bytes(v, sizeof(*v)); }
  bool Blob(std::vector<u8>* out, u64 limit = MAX_SECTION)
  {
    u64 size;
    if (!U64(&size) || size > limit || size > m_data.size() - m_pos)
      return false;
    out->assign(m_data.begin() + m_pos, m_data.begin() + m_pos + size);
    m_pos += size;
    return true;
  }
  bool AtEnd() const { return m_pos == m_data.size(); }

private:
  const std::vector<u8>& m_data;
  size_t m_pos = 0;
};

void Collect(const File::FSTEntry& entry, const std::string& prefix,
             std::vector<NandEntry>* entries, bool* ok)
{
  std::vector<const File::FSTEntry*> children;
  for (const auto& child : entry.children)
    children.push_back(&child);
  std::sort(children.begin(), children.end(),
            [](const auto* a, const auto* b) { return a->virtualName < b->virtualName; });
  for (const File::FSTEntry* child : children)
  {
    NandEntry out;
    out.path = prefix.empty() ? child->virtualName : prefix + "/" + child->virtualName;
    out.directory = child->isDirectory;
    if (!out.directory)
    {
      std::string data;
      if (!File::ReadFileToString(child->physicalName, data))
        *ok = false;
      out.data.assign(data.begin(), data.end());
    }
    const std::string path = out.path;
    entries->push_back(std::move(out));
    if (child->isDirectory)
      Collect(*child, path, entries, ok);
  }
}

bool SafeRelative(const std::string& path)
{
  if (path.empty() || path.size() > 1024 || path.front() == '/' || path.find('\\') != std::string::npos ||
      path.find('\0') != std::string::npos)
  {
    return false;
  }
  size_t start = 0;
  while (start <= path.size())
  {
    const size_t end = std::min(path.find('/', start), path.size());
    const std::string_view part(path.data() + start, end - start);
    if (part.empty() || part == "." || part == "..")
      return false;
    start = end + 1;
  }
  return true;
}
}  // namespace

bool ReadNandTree(const std::string& root, std::vector<NandEntry>* entries)
{
  entries->clear();
  if (!File::IsDirectory(root))
    return false;
  bool ok = true;
  Collect(File::ScanDirectoryTree(root, true), "", entries, &ok);
  return ok;
}

namespace
{
// The OS's error text, for logging a failed NAND replacement step.
std::string LastOsError()
{
#ifdef _WIN32
  return Common::GetLastErrorString();
#else
  return Common::LastStrerrorString();
#endif
}
}  // namespace

bool ReplaceNandTree(const std::string& root, const std::vector<NandEntry>& entries)
{
  if (root.empty())
    return false;
  for (const NandEntry& entry : entries)
  {
    if (!SafeRelative(entry.path))
    {
      ERROR_LOG_FMT(ROLLBACK, "Drop-in: the keyframe's NAND has an unsafe path");
      return false;
    }
  }
  if (File::IsDirectory(root) && !File::DeleteDirRecursively(root))
  {
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: couldn't delete the NAND at {}: {}", root, LastOsError());
    return false;
  }
  if (!File::CreateFullPath(root + "/"))
  {
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: couldn't create {}: {}", root, LastOsError());
    return false;
  }
  for (const NandEntry& entry : entries)
  {
    const std::string path = root + "/" + entry.path;
    if (entry.directory)
    {
      if (!File::CreateFullPath(path + "/"))
      {
        ERROR_LOG_FMT(ROLLBACK, "Drop-in: couldn't create {}: {}", path, LastOsError());
        return false;
      }
      continue;
    }
    File::CreateFullPath(path);
    File::IOFile file(path, "wb");
    if (!file || !file.WriteBytes(entry.data.data(), entry.data.size()))
    {
      ERROR_LOG_FMT(ROLLBACK, "Drop-in: couldn't write {}: {}", path, LastOsError());
      return false;
    }
  }
  return true;
}

namespace
{
size_t ReplayMetadataBytes(const ReplayFrame& frame)
{
  size_t bytes = sizeof(std::vector<Events::PortInfo>) + 96;
  for (const auto& port : *frame.ports)
    bytes += sizeof(Events::PortInfo) + 96 + port.name.size() + port.controls.size() + port.queue.size();
  return bytes;
}

bool ChargeMetadata(const ReplayFrame& frame, const ReplayFrame* previous, size_t* budget)
{
  if (previous && *previous->ports == *frame.ports)
    return true;
  const size_t bytes = ReplayMetadataBytes(frame);
  if (bytes > *budget)
    return false;
  *budget -= bytes;
  return true;
}

bool WriteReplayFrame(Writer& w, const ReplayFrame& frame)
{
  if (!frame.ports || frame.ports->size() > MAX_SEATS)
    return false;
  u32 seen = 0;
  for (const auto& port : *frame.ports)
  {
    if (port.port < 0 || port.port >= MAX_SEATS || (seen & (1u << port.port)) ||
        port.name.size() > 128 || port.controls.size() > MAX_CONTROLS || port.queue.size() > MAX_QUEUE)
      return false;
    seen |= 1u << port.port;
  }
  if (frame.header && (frame.header->mode > 2 || frame.header->ruleset > 2 ||
                       frame.header->coin > 1 || (frame.header->flags & ~3u)))
    return false;
  w.Bytes(frame.pads.data(), sizeof(frame.pads));
  w.U32(frame.ports ? static_cast<u32>(frame.ports->size()) : 0);
  if (frame.ports)
  {
    for (const auto& port : *frame.ports)
    {
      w.U32(port.port);
      w.U32(static_cast<u32>(port.name.size()));
      w.Bytes(port.name.data(), port.name.size());
      w.Blob(port.controls);
      w.Blob(port.queue);
    }
  }
  w.U32(frame.header.has_value());
  if (frame.header)
  {
    w.U32(frame.header->mode);
    w.U32(frame.header->ruleset);
    w.U32(frame.header->coin);
    w.U32(frame.header->flags);
    w.U32(frame.header->room);
  }
  w.U32(frame.clear_ready);
  return true;
}

bool ReadReplayFrame(Reader& r, ReplayFrame* frame)
{
  u32 count;
  if (!r.Bytes(frame->pads.data(), sizeof(frame->pads)) || !r.U32(&count) || count > MAX_SEATS)
    return false;
  auto ports = std::make_shared<std::vector<Events::PortInfo>>();
  ports->reserve(count);
  u32 seen = 0;
  for (u32 i = 0; i < count; ++i)
  {
    Events::PortInfo port;
    u32 seat, length;
    if (!r.U32(&seat) || seat >= MAX_SEATS || (seen & (1u << seat)) || !r.U32(&length) || length > 128)
      return false;
    seen |= 1u << seat;
    port.port = static_cast<int>(seat);
    port.remote = true;
    port.name.resize(length);
    if (!r.Bytes(port.name.data(), length) || !r.Blob(&port.controls, MAX_CONTROLS) ||
        !r.Blob(&port.queue, MAX_QUEUE))
      return false;
    ports->push_back(std::move(port));
  }
  frame->ports = std::move(ports);
  u32 header, clear;
  if (!r.U32(&header) || header > 1)
    return false;
  if (header)
  {
    u32 mode, ruleset, coin, flags, room;
    if (!r.U32(&mode) || mode > 2 || !r.U32(&ruleset) || ruleset > 2 ||
        !r.U32(&coin) || coin > 1 || !r.U32(&flags) || (flags & ~3u) || !r.U32(&room))
      return false;
    frame->header = ReplayHeader{static_cast<u8>(mode), static_cast<u8>(ruleset),
                                static_cast<u8>(coin), static_cast<u8>(flags), room};
  }
  if (!r.U32(&clear) || clear > 1)
    return false;
  frame->clear_ready = clear != 0;
  return true;
}
}  // namespace

std::vector<u8> PackKeyframe(int frame, const ReplayArchive& replay)
{
  try
  {
    if (frame < 0 || static_cast<size_t>(frame) > MAX_REPLAY_FRAMES ||
        replay.frames.size() < static_cast<size_t>(frame))
      return {};
    Writer w;
    w.Bytes(MAGIC, sizeof(MAGIC));
    w.U32(static_cast<u32>(frame));
    w.U64(replay.origin_hash);
    w.U64(replay.target_hash);
    size_t budget = MAX_RAW - (static_cast<size_t>(frame) + 1) * sizeof(ReplayFrame);
    const ReplayFrame* previous = nullptr;
    for (int i = 0; i < frame; ++i)
    {
      if (!WriteReplayFrame(w, replay.frames[i]))
        return {};
      if (!ChargeMetadata(replay.frames[i], previous, &budget))
        return {};
      previous = &replay.frames[i];
  }
  if (!WriteReplayFrame(w, replay.boundary) || !ChargeMetadata(replay.boundary, previous, &budget))
    return {};
  if (w.out.size() > MAX_RAW)
    return {};
  std::vector<u8> out(ZSTD_compressBound(w.out.size()));
  const size_t size = ZSTD_compress(out.data(), out.size(), w.out.data(), w.out.size(), LEVEL);
  if (ZSTD_isError(size))
    return {};
  out.resize(size);
  return out;
  }
  catch (const std::exception&)
  {
    return {};
  }
}

bool UnpackKeyframe(const std::vector<u8>& blob, int* frame, ReplayArchive* replay)
{
  try
  {
    const auto raw_size = ZSTD_getFrameContentSize(blob.data(), blob.size());
    if (raw_size == ZSTD_CONTENTSIZE_ERROR || raw_size == ZSTD_CONTENTSIZE_UNKNOWN ||
        raw_size > MAX_RAW)
      return false;
    std::vector<u8> raw(raw_size);
    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    if (!dctx)
      return false;
    ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, WINDOW_LOG);
    const size_t size = ZSTD_decompressDCtx(dctx, raw.data(), raw.size(), blob.data(), blob.size());
    ZSTD_freeDCtx(dctx);
    if (ZSTD_isError(size) || size != raw.size())
      return false;
    Reader r(raw);
    char magic[4];
    u32 count;
    ReplayArchive decoded;
    if (!r.Bytes(magic, sizeof(magic)) || std::memcmp(magic, MAGIC, sizeof(MAGIC)) != 0 ||
        !r.U32(&count) || count > MAX_REPLAY_FRAMES || !r.U64(&decoded.origin_hash) ||
        !r.U64(&decoded.target_hash))
      return false;
    if (static_cast<u64>(count + 1) * 44 > raw.size() - 24)
      return false;
    decoded.frames.reserve(count);
    size_t budget = MAX_RAW - (static_cast<size_t>(count) + 1) * sizeof(ReplayFrame);
    for (u32 i = 0; i < count; ++i)
    {
      ReplayFrame next;
      if (!ReadReplayFrame(r, &next))
        return false;
      if (!ChargeMetadata(next, decoded.frames.empty() ? nullptr : &decoded.frames.back(), &budget))
        return false;
      if (!decoded.frames.empty() && *decoded.frames.back().ports == *next.ports)
        next.ports = decoded.frames.back().ports;
      decoded.frames.push_back(std::move(next));
    }
    if (!ReadReplayFrame(r, &decoded.boundary) ||
        !ChargeMetadata(decoded.boundary, decoded.frames.empty() ? nullptr : &decoded.frames.back(), &budget) ||
        !r.AtEnd())
      return false;
    *frame = static_cast<int>(count);
    *replay = std::move(decoded);
    return true;
  }
  catch (const std::exception&)
  {
    return false;
  }
}


namespace
{
constexpr size_t GCM_IV = 12;
constexpr size_t GCM_TAG = 16;

std::string Aad(int frame)
{
  return fmt::format("orca-keyframe-{}", frame);
}

bool RandomBytes(u8* out, size_t size)
{
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);
  static constexpr char personal[] = "orca-keyframe";
  const bool ok =
      mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                            reinterpret_cast<const unsigned char*>(personal), sizeof(personal)) ==
          0 &&
      mbedtls_ctr_drbg_random(&drbg, out, size) == 0;
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
  return ok;
}

bool KeyFromHex(const std::string& hex, u8 key[32])
{
  if (hex.size() != 64)
    return false;
  for (size_t i = 0; i < 32; ++i)
  {
    unsigned value = 0;
    for (size_t k = 0; k < 2; ++k)
    {
      const char c = hex[i * 2 + k];
      const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      if (digit < 0)
        return false;
      value = value * 16 + static_cast<unsigned>(digit);
    }
    key[i] = static_cast<u8>(value);
  }
  return true;
}
}  // namespace

bool EncryptKeyframe(int frame, std::vector<u8>* blob, std::string* key_hex)
{
  u8 key[32], iv[GCM_IV];
  if (!RandomBytes(key, sizeof(key)) || !RandomBytes(iv, sizeof(iv)))
    return false;
  std::vector<u8> out(GCM_IV + blob->size() + GCM_TAG);
  std::copy(iv, iv + GCM_IV, out.begin());
  const std::string aad = Aad(frame);
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  const bool ok =
      mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
      mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, blob->size(), iv, GCM_IV,
                                reinterpret_cast<const unsigned char*>(aad.data()), aad.size(),
                                blob->data(), out.data() + GCM_IV, GCM_TAG,
                                out.data() + GCM_IV + blob->size()) == 0;
  mbedtls_gcm_free(&gcm);
  if (!ok)
    return false;
  key_hex->clear();
  for (u8 b : key)
    *key_hex += fmt::format("{:02x}", b);
  *blob = std::move(out);
  return true;
}

bool DecryptKeyframe(int frame, const std::string& key_hex, std::vector<u8>* blob)
{
  u8 key[32];
  if (!KeyFromHex(key_hex, key) || blob->size() < GCM_IV + GCM_TAG)
    return false;
  const size_t length = blob->size() - GCM_IV - GCM_TAG;
  std::vector<u8> out(length);
  const std::string aad = Aad(frame);
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  const bool ok =
      mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0 &&
      mbedtls_gcm_auth_decrypt(&gcm, length, blob->data(), GCM_IV,
                               reinterpret_cast<const unsigned char*>(aad.data()), aad.size(),
                               blob->data() + GCM_IV + length, GCM_TAG, blob->data() + GCM_IV,
                               out.data()) == 0;
  mbedtls_gcm_free(&gcm);
  if (!ok)
    return false;
  *blob = std::move(out);
  return true;
}

u64 NandHash(const std::vector<u8>& data)
{
  return XXH3_64bits(data.data(), data.size());
}

std::map<std::string, u64> HashNandTree(const std::string& root)
{
  std::map<std::string, u64> hashes;
  std::vector<NandEntry> entries;
  if (!ReadNandTree(root, &entries))
    return hashes;
  for (const NandEntry& entry : entries)
  {
    if (!entry.directory)
      hashes[entry.path] = NandHash(entry.data);
  }
  return hashes;
}

bool ResolveNandReferences(const std::string& root, std::vector<NandEntry>* entries,
                           std::string* error)
{
  for (NandEntry& entry : *entries)
  {
    if (!entry.reference)
      continue;
    std::string data;
    if (!File::ReadFileToString(root + "/" + entry.path, data))
    {
      *error = entry.path + " is missing";
      return false;
    }
    entry.data.assign(data.begin(), data.end());
    if (NandHash(entry.data) != entry.hash)
    {
      *error = entry.path + " differs";
      return false;
    }
    entry.reference = false;
  }
  return true;
}

std::string KeyframeHash(const std::vector<u8>& blob)
{
  return fmt::format("{:016x}", XXH3_64bits(blob.data(), blob.size()));
}

namespace
{
// LastRefusal, per thread: the job and the download threads each read their own.
thread_local std::string t_last_refusal;

// A folder shared by two Orcas on one machine (tests): the host writes <id>.okf, the joiner reads
// it.
class LocalKeyframeStore final : public KeyframeStore
{
public:
  explicit LocalKeyframeStore(std::string dir) : m_dir(std::move(dir))
  {
    File::CreateFullPath(m_dir + "/");
  }
  bool Put(const KeyframeInfo& info, const std::vector<u8>& data, std::string* error) override
  {
    if (!ValidReplayId(info.id) || data.size() > MAX_KEYFRAME_BYTES || data.size() != info.size)
    {
      *error = "invalid replay offer";
      return false;
    }
    const std::string path = Path(info.id);
    const std::string temp = path + ".part";
    File::IOFile file(temp, "wb");
    if (!file || !file.WriteBytes(data.data(), data.size()) || !file.Close() ||
        !File::Rename(temp, path))
    {
      *error = "could not write " + path;
      return false;
    }
    return true;
  }
  std::optional<std::vector<u8>> Get(const KeyframeInfo& info,
                                     const std::function<bool(u64, u64)>& progress,
                                     std::string* error) override
  {
    if (!ValidReplayId(info.id) || info.size == 0 || info.size > MAX_KEYFRAME_BYTES)
    {
      *error = "invalid replay offer";
      return std::nullopt;
    }
    File::IOFile file(Path(info.id), "rb");
    if (!file || file.GetSize() != info.size)
    {
      *error = "no keyframe " + info.id;
      return std::nullopt;
    }
    std::vector<u8> data(info.size);
    constexpr u64 CHUNK = 4 << 20;
    for (u64 done = 0; done < info.size;)
    {
      const u64 n = std::min(CHUNK, info.size - done);
      if (!file.ReadBytes(data.data() + done, n))
      {
        *error = "could not read keyframe " + info.id;
        return std::nullopt;
      }
      done += n;
      if (!progress(done, info.size))
      {
        *error = "cancelled";
        return std::nullopt;
      }
    }
    return data;
  }
  void Delete(const std::string& id) override
  {
    if (ValidReplayId(id) && Orca::GetEnv("ORCA_TEST_KEEP_KEYFRAMES") != "1")
      File::Delete(Path(id));
  }

private:
  std::string Path(const std::string& id) const { return m_dir + "/" + id + ".okf"; }
  std::string m_dir;
};

// YouGame's keyframe endpoint: PUT, GET and DELETE on <store_url>/<id> with "Authorization: Ticket
// <room ticket>". A 401 with code "ticket", or a 403 "signed_out" (the player may have signed in
// since), retries with a new ticket; network errors, 429 and 5xx retry with backoff; other refusals
// are final. A 409 "exists" on PUT means an earlier attempt landed but its reply was lost.
class HttpKeyframeStore final : public KeyframeStore
{
public:
  explicit HttpKeyframeStore(TicketSource tickets) : m_tickets(std::move(tickets)) {}
  ~HttpKeyframeStore() override
  {
    std::lock_guard lock(m_deletes_mutex);
    for (std::thread& t : m_deletes)
      t.join();
  }

  bool Put(const KeyframeInfo& info, const std::vector<u8>& data, std::string* error) override
  {
    if (!ValidReplayId(info.id) || data.size() > MAX_KEYFRAME_BYTES || data.size() != info.size)
    {
      *error = "invalid replay offer";
      return false;
    }
    return WithRetries(error, [&](const std::string& ticket, const std::string& base,
                                  Result* result) {
      *result = Request("PUT", base + "/" + info.id,
                        {"Authorization: Ticket " + ticket,
                         "Content-Type: application/octet-stream",
                         fmt::format("X-Orca-Frame: {}", info.frame), "X-Orca-Hash: " + info.hash},
                        &data, nullptr, nullptr);
      if (result->status == 409 && result->code == "exists")
        result->status = 200;
    });
  }

  std::optional<std::vector<u8>> Get(const KeyframeInfo& info,
                                     const std::function<bool(u64, u64)>& progress,
                                     std::string* error) override
  {
    if (!ValidReplayId(info.id) || info.size == 0 || info.size > MAX_KEYFRAME_BYTES)
    {
      *error = fmt::format("a keyframe of {} bytes", info.size);
      return std::nullopt;
    }
    std::vector<u8> body;
    const bool ok = WithRetries(error, [&](const std::string& ticket, const std::string& base,
                                           Result* result) {
      body.clear();
      body.reserve(info.size);
      *result = Request("GET", base + "/" + info.id, {"Authorization: Ticket " + ticket}, nullptr,
                        &body, [&](u64 done, u64) { return progress(done, info.size); });
    });
    if (!ok)
      return std::nullopt;
    if (body.size() != info.size)
    {
      *error = fmt::format("the keyframe arrived with {} bytes of {}", body.size(), info.size);
      return std::nullopt;
    }
    return body;
  }

  void Delete(const std::string& id) override
  {
    if (!ValidReplayId(id))
      return;
    // Best effort, off the caller's thread (the CPU thread calls this right after a load); the
    // site's cleanup catches anything missed. Copy the ticket now, since the room may be gone when
    // the thread runs.
    std::string ticket, base, error;
    if (!m_tickets || !m_tickets(false, &ticket, &base, &error))
    {
      WARN_LOG_FMT(NETPLAY, "Orca keyframe: can't delete {}: {}", id, error);
      return;
    }
    std::lock_guard lock(m_deletes_mutex);
    m_deletes.emplace_back([ticket, base, id] {
      for (int attempt = 0; attempt < 3; ++attempt)
      {
        const Result result = Request("DELETE", base + "/" + id, {"Authorization: Ticket " + ticket},
                                      nullptr, nullptr, nullptr, nullptr);
        if ((result.status >= 200 && result.status < 300) || result.status == 404)
          return;
        if (result.status != 0 && result.status != 429 && result.status < 500)
        {
          WARN_LOG_FMT(NETPLAY, "Orca keyframe: deleting {} refused (HTTP {})", id, result.status);
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500 << attempt));
      }
    });
  }

private:
  void Cancel() override { m_cancel = true; }

  struct Result
  {
    long status = 0;  // 0: no HTTP response
    std::string error;
    std::string code;  // YouGame's {"code"} in a refusal
    bool final = false;  // not worth retrying
  };

  static constexpr int ATTEMPTS = 4;

  bool WithRetries(std::string* error,
                   const std::function<void(const std::string&, const std::string&, Result*)>& op)
  {
    t_last_refusal.clear();
    bool fresh = false;
    for (int attempt = 0; attempt < ATTEMPTS; ++attempt)
    {
      if (attempt > 0 && !fresh)
      {
        const auto until =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500 << (attempt - 1));
        while (!m_cancel && std::chrono::steady_clock::now() < until)
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      if (m_cancel)
      {
        *error = "cancelled";
        return false;
      }
      std::string ticket, base, ticket_error;
      if (!m_tickets || !m_tickets(fresh, &ticket, &base, &ticket_error))
      {
        *error = "no ticket: " + ticket_error;
        return false;
      }
      Result result;
      op(ticket, base, &result);
      if (result.status >= 200 && result.status < 300)
      {
        error->clear();
        return true;
      }
      *error = result.status ? fmt::format("HTTP {}{}{}", result.status,
                                           result.error.empty() ? "" : ": ", result.error) :
                               result.error;
      // Expired or unknown ticket, or one minted before the player signed in: retry once with a
      // new one.
      if (((result.status == 401 && result.code == "ticket") ||
           (result.status == 403 && result.code == "signed_out")) &&
          !fresh)
      {
        fresh = true;
        continue;
      }
      fresh = false;
      // Refusals (wrong room, too large, gone, room full) are final; 429, 5xx and network errors
      // retry.
      const bool retry = !result.final && (result.status == 0 || result.status == 429 ||
                                           result.status >= 500);
      if (!retry)
      {
        // Only YouGame's own refusals carry a code; a bare 4xx (an edge page) may pass next time.
        if (result.status >= 400)
          t_last_refusal = result.code;
        return false;
      }
      WARN_LOG_FMT(NETPLAY, "Orca keyframe: {}; trying again", *error);
    }
    return false;
  }

  struct Transfer
  {
    CURL* curl = nullptr;
    const std::vector<u8>* upload = nullptr;
    size_t sent = 0;
    std::vector<u8>* download = nullptr;
    std::vector<u8>* error_body = nullptr;
    u64 limit = 0;
    std::function<bool(u64, u64)> progress;
    const std::atomic<bool>* cancel = nullptr;
  };

  static int OnProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
  {
    const auto* t = static_cast<Transfer*>(user);
    return t->cancel && t->cancel->load() ? 1 : 0;
  }

  static size_t OnRead(char* buffer, size_t size, size_t count, void* user)
  {
    auto* t = static_cast<Transfer*>(user);
    const size_t n = std::min(size * count, t->upload->size() - t->sent);
    std::memcpy(buffer, t->upload->data() + t->sent, n);
    t->sent += n;
    return n;
  }

  static size_t OnWrite(char* data, size_t size, size_t count, void* user)
  {
    try
    {
      auto* t = static_cast<Transfer*>(user);
      const size_t n = size * count;
      // A refusal's body is YouGame's {error, code}, not the keyframe.
      long status = 0;
      curl_easy_getinfo(t->curl, CURLINFO_RESPONSE_CODE, &status);
      if (status < 200 || status >= 300 || !t->download)
      {
        if (t->error_body->size() + n > 64 * 1024)
          return 0;
        t->error_body->insert(t->error_body->end(), data, data + n);
        return n;
    }
    if (t->download->size() + n > t->limit)
      return 0;  // abort: larger than any keyframe
    t->download->insert(t->download->end(), data, data + n);
    if (t->progress && !t->progress(t->download->size(), 0))
      return 0;
    return n;
    }
    catch (const std::exception&)
    {
      return 0;
    }
  }

  Result Request(const char* method, const std::string& url,
                 const std::vector<std::string>& headers, const std::vector<u8>* upload,
                 std::vector<u8>* download, std::function<bool(u64, u64)> progress) const
  {
    return Request(method, url, headers, upload, download, std::move(progress), &m_cancel);
  }

  static Result Request(const char* method, const std::string& url,
                        const std::vector<std::string>& headers, const std::vector<u8>* upload,
                        std::vector<u8>* download, std::function<bool(u64, u64)> progress,
                        const std::atomic<bool>* cancel)
  {
    Result result;
    CURL* curl = curl_easy_init();
    if (!curl)
    {
      result.error = "no HTTP client";
      return result;
    }
    curl_slist* list = nullptr;
    for (const std::string& h : headers)
      list = curl_slist_append(list, h.c_str());
    std::vector<u8> error_body;
    Transfer t;
    t.curl = curl;
    t.upload = upload;
    t.download = download;
    t.error_body = &error_body;
    t.limit = MAX_KEYFRAME_BYTES;
    t.progress = std::move(progress);
    t.cancel = cancel;
    Common::ConfigureCurlTLS(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &OnProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    // A transfer may take minutes on a slow link, but must not stall for 20 s.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &OnWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    if (std::string_view(method) == "PUT")
    {
      curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
      curl_easy_setopt(curl, CURLOPT_READFUNCTION, &OnRead);
      curl_easy_setopt(curl, CURLOPT_READDATA, &t);
      curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                       static_cast<curl_off_t>(upload ? upload->size() : 0));
    }
    else if (std::string_view(method) != "GET")
    {
      curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    }
    const CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK)
    {
      result.error = curl_easy_strerror(code);
      // Aborts (the player gave up, emulation stopping, an oversized body) are not worth retrying.
      result.final = code == CURLE_WRITE_ERROR || code == CURLE_ABORTED_BY_CALLBACK ||
                     (cancel && cancel->load());
      if (result.status >= 200 && result.status < 300)
        result.status = 0;
    }
    else if (!(result.status >= 200 && result.status < 300) && !error_body.empty())
    {
      // YouGame's refusals are {"error": "...", "code": "..."}.
      picojson::value json;
      const std::string text(error_body.begin(), error_body.end());
      if (picojson::parse(json, text).empty() && json.is<picojson::object>())
      {
        const auto& o = json.get<picojson::object>();
        if (const auto it = o.find("error"); it != o.end() && it->second.is<std::string>())
          result.error = it->second.get<std::string>().substr(0, 200);
        if (const auto it = o.find("code"); it != o.end() && it->second.is<std::string>())
          result.code = it->second.get<std::string>().substr(0, 32);
      }
    }
    return result;
  }

  TicketSource m_tickets;
  std::atomic<bool> m_cancel{false};
  std::mutex m_deletes_mutex;
  std::vector<std::thread> m_deletes;
};
}  // namespace

std::unique_ptr<KeyframeStore> MakeHttpKeyframeStore(TicketSource tickets)
{
  return std::make_unique<HttpKeyframeStore>(std::move(tickets));
}

std::unique_ptr<KeyframeStore> MakeKeyframeStore(TicketSource tickets)
{
  const std::string dir = Orca::GetEnv("ORCA_TEST_KEYFRAME_DIR");
  if (!dir.empty())
    return std::make_unique<LocalKeyframeStore>(dir);
  return MakeHttpKeyframeStore(std::move(tickets));
}

std::string LastRefusal()
{
  return t_last_refusal;
}
}  // namespace Orca::Net
