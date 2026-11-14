#include "common/shm_pool.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace soeasy {
namespace common {

namespace {

// 64-bit FNV-1a checksum for rapid zero-copy slot payload verification
uint64_t FastChecksum64(const uint8_t *data, size_t len) {
  constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
  constexpr uint64_t kFnvPrime = 0x100000001b3ULL;
  uint64_t hash = kFnvOffsetBasis;
  for (size_t i = 0; i < len; ++i) {
    hash ^= static_cast<uint64_t>(data[i]);
    hash *= kFnvPrime;
  }
  return hash;
}

size_t DetectOsPageSize() {
#ifdef _WIN32
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwPageSize > 0 ? static_cast<size_t>(si.dwPageSize) : 4096;
#else
  long ps = sysconf(_SC_PAGESIZE);
  return ps > 0 ? static_cast<size_t>(ps) : 4096;
#endif
}

size_t AlignUp(size_t value, size_t alignment) {
  if (alignment == 0) return value;
  return ((value + alignment - 1) / alignment) * alignment;
}

}  // namespace

ShmSlotPool::ShmSlotPool() : page_size_(std::max<size_t>(sizeof(SlotHeader), DetectOsPageSize())) {}

ShmSlotPool::~ShmSlotPool() {
  Close();
}

size_t ShmSlotPool::GetPayloadAlignedSize() const {
  return AlignUp(slot_capacity_, page_size_);
}

size_t ShmSlotPool::GetSlotStride() const {
  // Isolate SlotHeader on its own leading OS page so payload pages can be MADV_REMOVE-reclaimed safely.
  return page_size_ + GetPayloadAlignedSize();
}

uint8_t *ShmSlotPool::GetSlotBase(uint32_t slot_id) const {
  if (!base_addr_ || slot_id >= slot_count_) {
    return nullptr;
  }
  return base_addr_ + (static_cast<size_t>(slot_id) * GetSlotStride());
}

SlotHeader *ShmSlotPool::GetSlotHeader(uint32_t slot_id) const {
  return reinterpret_cast<SlotHeader *>(GetSlotBase(slot_id));
}

uint8_t *ShmSlotPool::GetSlotData(uint32_t slot_id) const {
  uint8_t *base = GetSlotBase(slot_id);
  if (!base) {
    return nullptr;
  }
  return base + page_size_;
}

bool ShmSlotPool::Create(const std::string &shm_name, size_t slot_count, size_t slot_capacity) {
  Close();

  shm_name_ = shm_name;
  slot_count_ = slot_count;
  slot_capacity_ = slot_capacity;
  total_shm_size_ = slot_count_ * GetSlotStride();
  is_creator_ = true;

#ifdef _WIN32
  std::string win_name = shm_name_;
  for (char &c : win_name) {
    if (c == '/') c = '_';
  }
  if (win_name.rfind("Local\\", 0) != 0 && win_name.rfind("Global\\", 0) != 0) {
    win_name = "Local\\" + win_name;
  }

  DWORD size_high = static_cast<DWORD>((total_shm_size_ >> 32) & 0xFFFFFFFF);
  DWORD size_low = static_cast<DWORD>(total_shm_size_ & 0xFFFFFFFF);

  HANDLE hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, size_high, size_low, win_name.c_str());
  if (!hMap) {
    return false;
  }
  win_map_handle_ = hMap;

  void *addr = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, total_shm_size_);
  if (!addr) {
    CloseHandle(hMap);
    win_map_handle_ = nullptr;
    return false;
  }

  base_addr_ = static_cast<uint8_t *>(addr);
#else
  // Remove stale shm segment if leftover from a crash
  shm_unlink(shm_name_.c_str());

  shm_fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_RDWR | O_EXCL, 0660);
  if (shm_fd_ < 0) {
    return false;
  }

  if (ftruncate(shm_fd_, static_cast<off_t>(total_shm_size_)) != 0) {
    close(shm_fd_);
    shm_fd_ = -1;
    shm_unlink(shm_name_.c_str());
    return false;
  }

  void *addr = mmap(nullptr, total_shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
  if (addr == MAP_FAILED) {
    close(shm_fd_);
    shm_fd_ = -1;
    shm_unlink(shm_name_.c_str());
    return false;
  }

  base_addr_ = static_cast<uint8_t *>(addr);
#endif

  // Leave payload pages untouched (lazy zero-fill on demand fault); initialize only SlotHeader pages.
  for (uint32_t i = 0; i < slot_count_; ++i) {
    SlotHeader *header = GetSlotHeader(i);
    header->state.store(kSlotFree, std::memory_order_relaxed);
    header->slot_id = i;
    header->data_bytes = 0;
    header->checksum = 0;
    header->last_heartbeat_ms.store(0, std::memory_order_relaxed);
    std::memset(header->reserved, 0, sizeof(header->reserved));
  }
  has_dirty_pages_.store(false, std::memory_order_release);

  return true;
}

bool ShmSlotPool::Attach(const std::string &shm_name, size_t slot_count, size_t slot_capacity) {
  Close();

  shm_name_ = shm_name;
  slot_count_ = slot_count;
  slot_capacity_ = slot_capacity;
  total_shm_size_ = slot_count_ * GetSlotStride();
  is_creator_ = false;

#ifdef _WIN32
  std::string win_name = shm_name_;
  for (char &c : win_name) {
    if (c == '/') c = '_';
  }
  if (win_name.rfind("Local\\", 0) != 0 && win_name.rfind("Global\\", 0) != 0) {
    win_name = "Local\\" + win_name;
  }

  HANDLE hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, win_name.c_str());
  if (!hMap) {
    return false;
  }
  win_map_handle_ = hMap;

  void *addr = MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, total_shm_size_);
  if (!addr) {
    CloseHandle(hMap);
    win_map_handle_ = nullptr;
    return false;
  }

  base_addr_ = static_cast<uint8_t *>(addr);
#else
  shm_fd_ = shm_open(shm_name_.c_str(), O_RDWR, 0660);
  if (shm_fd_ < 0) {
    return false;
  }

  void *addr = mmap(nullptr, total_shm_size_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
  if (addr == MAP_FAILED) {
    close(shm_fd_);
    shm_fd_ = -1;
    return false;
  }

  base_addr_ = static_cast<uint8_t *>(addr);
#endif
  return true;
}

void ShmSlotPool::Close() {
#ifdef _WIN32
  if (base_addr_) {
    UnmapViewOfFile(base_addr_);
    base_addr_ = nullptr;
  }
  if (win_map_handle_) {
    CloseHandle(reinterpret_cast<HANDLE>(win_map_handle_));
    win_map_handle_ = nullptr;
  }
#else
  if (base_addr_) {
    munmap(base_addr_, total_shm_size_);
    base_addr_ = nullptr;
  }
  if (shm_fd_ >= 0) {
    close(shm_fd_);
    shm_fd_ = -1;
  }
  if (is_creator_ && !shm_name_.empty()) {
    shm_unlink(shm_name_.c_str());
  }
#endif
  is_creator_ = false;
  has_dirty_pages_.store(false, std::memory_order_relaxed);
  shm_name_.clear();
  slot_count_ = 0;
  slot_capacity_ = 0;
  total_shm_size_ = 0;
}

bool ShmSlotPool::AllocateSlot(uint32_t *out_slot_id, uint8_t **out_data_ptr) {
  if (!base_addr_ || !out_slot_id || !out_data_ptr) {
    return false;
  }

  for (uint32_t i = 0; i < slot_count_; ++i) {
    SlotHeader *header = GetSlotHeader(i);
    uint32_t expected = kSlotFree;
    if (header->state.compare_exchange_strong(expected, kSlotAllocated, std::memory_order_acq_rel)) {
      header->data_bytes = 0;
      header->checksum = 0;
      uint64_t now_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
              .count();
      header->last_heartbeat_ms.store(now_ms, std::memory_order_relaxed);
      has_dirty_pages_.store(true, std::memory_order_release);
      *out_slot_id = i;
      *out_data_ptr = GetSlotData(i);
      return true;
    }
  }

  return false;
}

bool ShmSlotPool::CommitWrite(uint32_t slot_id, size_t data_bytes) {
  if (!base_addr_ || slot_id >= slot_count_ || data_bytes > slot_capacity_) {
    return false;
  }

  SlotHeader *header = GetSlotHeader(slot_id);
  const uint8_t *data = GetSlotData(slot_id);
  header->data_bytes = data_bytes;
  header->checksum = FastChecksum64(data, data_bytes);

  header->state.store(kSlotWritten, std::memory_order_release);
  return true;
}

bool ShmSlotPool::ReadSlot(uint32_t slot_id, const uint8_t **out_data_ptr, size_t *out_data_bytes) {
  if (!base_addr_ || slot_id >= slot_count_ || !out_data_ptr || !out_data_bytes) {
    return false;
  }

  SlotHeader *header = GetSlotHeader(slot_id);
  uint32_t cur_state = header->state.load(std::memory_order_acquire);
  if (cur_state != kSlotWritten) {
    return false;
  }

  size_t bytes = header->data_bytes;
  if (bytes > slot_capacity_) {
    return false;
  }

  const uint8_t *data = GetSlotData(slot_id);
  uint64_t expected_checksum = header->checksum;
  if (FastChecksum64(data, bytes) != expected_checksum) {
    return false;  // Memory corruption detected
  }

  *out_data_ptr = data;
  *out_data_bytes = bytes;
  return true;
}

void ShmSlotPool::ReleaseSlot(uint32_t slot_id) {
  if (!base_addr_ || slot_id >= slot_count_) {
    return;
  }
  SlotHeader *header = GetSlotHeader(slot_id);
  header->state.store(kSlotFree, std::memory_order_release);
}

void ShmSlotPool::TouchHeartbeat(uint32_t slot_id) {
  if (!base_addr_ || slot_id >= slot_count_) {
    return;
  }
  SlotHeader *header = GetSlotHeader(slot_id);
  uint64_t now_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count();
  header->last_heartbeat_ms.store(now_ms, std::memory_order_relaxed);
}

uint64_t ShmSlotPool::GetSlotLastHeartbeat(uint32_t slot_id) const {
  if (!base_addr_ || slot_id >= slot_count_) {
    return 0;
  }
  SlotHeader *header = GetSlotHeader(slot_id);
  return header->last_heartbeat_ms.load(std::memory_order_relaxed);
}

uint8_t *ShmSlotPool::GetWritableSlotData(uint32_t slot_id) {
  return GetSlotData(slot_id);
}

bool ShmSlotPool::ReclaimIdlePages() {
  if (!base_addr_ || !is_creator_ || slot_count_ == 0 || slot_capacity_ == 0) {
    return false;
  }

  for (uint32_t i = 0; i < slot_count_; ++i) {
    SlotHeader *header = GetSlotHeader(i);
    if (!header || header->state.load(std::memory_order_acquire) != kSlotFree) {
      return false;
    }
  }

  if (!has_dirty_pages_.load(std::memory_order_acquire)) {
    return true;
  }

#ifdef __linux__
  const size_t payload_bytes = GetPayloadAlignedSize();
  for (uint32_t i = 0; i < slot_count_; ++i) {
    uint8_t *data_ptr = GetSlotData(i);
    if (data_ptr && payload_bytes > 0) {
      if (madvise(data_ptr, payload_bytes, MADV_REMOVE) != 0) {
        return false;
      }
    }
  }
#endif

  has_dirty_pages_.store(false, std::memory_order_release);
  return true;
}

}  // namespace common
}  // namespace soeasy
