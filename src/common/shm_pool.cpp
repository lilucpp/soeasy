#include "common/shm_pool.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace soeasy {
namespace common {

namespace {

// 64-bit FNV-1a checksum for rapid zero-copy slot payload verification
uint64_t FastChecksum64(const uint8_t* data, size_t len) {
  constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
  constexpr uint64_t kFnvPrime = 0x100000001b3ULL;
  uint64_t hash = kFnvOffsetBasis;
  for (size_t i = 0; i < len; ++i) {
    hash ^= static_cast<uint64_t>(data[i]);
    hash *= kFnvPrime;
  }
  return hash;
}

}  // namespace

ShmSlotPool::ShmSlotPool() = default;

ShmSlotPool::~ShmSlotPool() {
  Close();
}

size_t ShmSlotPool::GetSlotStride() const {
  // Stride = sizeof(SlotHeader) + slot_capacity_, aligned to 64 bytes
  size_t raw = sizeof(SlotHeader) + slot_capacity_;
  return (raw + 63) & ~63ULL;
}

uint8_t* ShmSlotPool::GetSlotBase(uint32_t slot_id) const {
  if (!base_addr_ || slot_id >= slot_count_) {
    return nullptr;
  }
  return base_addr_ + (static_cast<size_t>(slot_id) * GetSlotStride());
}

SlotHeader* ShmSlotPool::GetSlotHeader(uint32_t slot_id) const {
  return reinterpret_cast<SlotHeader*>(GetSlotBase(slot_id));
}

uint8_t* ShmSlotPool::GetSlotData(uint32_t slot_id) const {
  uint8_t* base = GetSlotBase(slot_id);
  if (!base) {
    return nullptr;
  }
  return base + sizeof(SlotHeader);
}

bool ShmSlotPool::Create(const std::string& shm_name, size_t slot_count,
                         size_t slot_capacity) {
  Close();

  shm_name_ = shm_name;
  slot_count_ = slot_count;
  slot_capacity_ = slot_capacity;
  total_shm_size_ = slot_count_ * GetSlotStride();
  is_creator_ = true;

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

  void* addr = mmap(nullptr, total_shm_size_, PROT_READ | PROT_WRITE,
                    MAP_SHARED, shm_fd_, 0);
  if (addr == MAP_FAILED) {
    close(shm_fd_);
    shm_fd_ = -1;
    shm_unlink(shm_name_.c_str());
    return false;
  }

  base_addr_ = static_cast<uint8_t*>(addr);
  std::memset(base_addr_, 0, total_shm_size_);

  for (uint32_t i = 0; i < slot_count_; ++i) {
    SlotHeader* header = GetSlotHeader(i);
    header->state.store(kSlotFree, std::memory_order_relaxed);
    header->slot_id = i;
    header->data_bytes = 0;
    header->checksum = 0;
  }

  return true;
}

bool ShmSlotPool::Attach(const std::string& shm_name, size_t slot_count,
                         size_t slot_capacity) {
  Close();

  shm_name_ = shm_name;
  slot_count_ = slot_count;
  slot_capacity_ = slot_capacity;
  total_shm_size_ = slot_count_ * GetSlotStride();
  is_creator_ = false;

  shm_fd_ = shm_open(shm_name_.c_str(), O_RDWR, 0660);
  if (shm_fd_ < 0) {
    return false;
  }

  void* addr = mmap(nullptr, total_shm_size_, PROT_READ | PROT_WRITE,
                    MAP_SHARED, shm_fd_, 0);
  if (addr == MAP_FAILED) {
    close(shm_fd_);
    shm_fd_ = -1;
    return false;
  }

  base_addr_ = static_cast<uint8_t*>(addr);
  return true;
}

void ShmSlotPool::Close() {
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
    is_creator_ = false;
  }
  shm_name_.clear();
  slot_count_ = 0;
  slot_capacity_ = 0;
  total_shm_size_ = 0;
}

bool ShmSlotPool::AllocateSlot(uint32_t* out_slot_id, uint8_t** out_data_ptr) {
  if (!base_addr_ || !out_slot_id || !out_data_ptr) {
    return false;
  }

  for (uint32_t i = 0; i < slot_count_; ++i) {
    SlotHeader* header = GetSlotHeader(i);
    uint32_t expected = kSlotFree;
    if (header->state.compare_exchange_strong(
            expected, kSlotAllocated, std::memory_order_acq_rel)) {
      header->data_bytes = 0;
      header->checksum = 0;
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

  SlotHeader* header = GetSlotHeader(slot_id);
  const uint8_t* data = GetSlotData(slot_id);
  header->data_bytes = data_bytes;
  header->checksum = FastChecksum64(data, data_bytes);

  header->state.store(kSlotWritten, std::memory_order_release);
  return true;
}

bool ShmSlotPool::ReadSlot(uint32_t slot_id, const uint8_t** out_data_ptr,
                           size_t* out_data_bytes) {
  if (!base_addr_ || slot_id >= slot_count_ || !out_data_ptr || !out_data_bytes) {
    return false;
  }

  SlotHeader* header = GetSlotHeader(slot_id);
  uint32_t cur_state = header->state.load(std::memory_order_acquire);
  if (cur_state != kSlotWritten) {
    return false;
  }

  size_t bytes = header->data_bytes;
  if (bytes > slot_capacity_) {
    return false;
  }

  const uint8_t* data = GetSlotData(slot_id);
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
  SlotHeader* header = GetSlotHeader(slot_id);
  header->state.store(kSlotFree, std::memory_order_release);
}

uint8_t* ShmSlotPool::GetWritableSlotData(uint32_t slot_id) {
  return GetSlotData(slot_id);
}

}  // namespace common
}  // namespace soeasy
