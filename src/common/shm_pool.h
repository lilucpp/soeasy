#ifndef SOEASY_SRC_COMMON_SHM_POOL_H_
#define SOEASY_SRC_COMMON_SHM_POOL_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace soeasy {
namespace common {

enum SlotState : uint32_t {
  kSlotFree = 0,
  kSlotAllocated = 1,
  kSlotWritten = 2,
  kSlotReading = 3,
};

#pragma pack(push, 8)
struct SlotHeader {
  std::atomic<uint32_t> state;
  uint32_t slot_id;
  uint64_t data_bytes;
  uint64_t checksum;
  uint8_t reserved[40];  // Cache line padding (64 bytes total)
};
#pragma pack(pop)

static_assert(sizeof(SlotHeader) == 64, "SlotHeader must be exactly 64 bytes");

class ShmSlotPool {
 public:
  static constexpr size_t kDefaultSlotCapacity = 8 * 1024 * 1024;  // 8MB per slot
  static constexpr size_t kDefaultSlotCount = 8;                   // 8 slots (64MB pool)

  ShmSlotPool();
  ~ShmSlotPool();

  // Disallow copy and assign
  ShmSlotPool(const ShmSlotPool&) = delete;
  ShmSlotPool& operator=(const ShmSlotPool&) = delete;

  /**
   * @brief Create a brand new POSIX shared memory pool (called by Master service).
   *
   * @param shm_name Unique SHM segment name (e.g. "/soeasy_worker_shm_1234")
   * @param slot_count Number of slots to partition the memory into
   * @param slot_capacity Maximum usable payload bytes per slot
   * @return true on success, false on error
   */
  bool Create(const std::string& shm_name,
              size_t slot_count = kDefaultSlotCount,
              size_t slot_capacity = kDefaultSlotCapacity);

  /**
   * @brief Attach to an existing shared memory pool (called by Worker processes).
   *
   * @param shm_name Shared memory segment name created by Master
   * @param slot_count Number of slots in the pool
   * @param slot_capacity Maximum usable payload bytes per slot
   * @return true on success, false on error
   */
  bool Attach(const std::string& shm_name,
              size_t slot_count = kDefaultSlotCount,
              size_t slot_capacity = kDefaultSlotCapacity);

  /**
   * @brief Detach and release mapping. If creator, unlinks SHM segment.
   */
  void Close();

  /**
   * @brief Allocate a free slot for writing extracted document content.
   *
   * @param out_slot_id Receives allocated slot index (0..slot_count-1)
   * @param out_data_ptr Receives pointer to writable data region
   * @return true if a slot was acquired, false if pool is exhausted
   */
  bool AllocateSlot(uint32_t* out_slot_id, uint8_t** out_data_ptr);

  /**
   * @brief Mark slot as written and ready for Master consumption.
   *
   * @param slot_id Slot index
   * @param data_bytes Actual number of bytes written into the slot
   * @return true on success
   */
  bool CommitWrite(uint32_t slot_id, size_t data_bytes);

  /**
   * @brief Access readable data in a written slot.
   *
   * @param slot_id Slot index
   * @param out_data_ptr Receives pointer to slot payload
   * @param out_data_bytes Receives actual payload byte count
   * @return true on success
   */
  bool ReadSlot(uint32_t slot_id, const uint8_t** out_data_ptr,
                size_t* out_data_bytes);

  /**
   * @brief Free an allocated or read slot back to the available pool.
   *
   * @param slot_id Slot index
   */
  void ReleaseSlot(uint32_t slot_id);

  /**
   * @brief Get pointer to writable payload buffer for a slot.
   */
  uint8_t* GetWritableSlotData(uint32_t slot_id);

  const std::string& shm_name() const { return shm_name_; }
  size_t slot_count() const { return slot_count_; }
  size_t slot_capacity() const { return slot_capacity_; }
  bool is_valid() const { return base_addr_ != nullptr; }

 private:
  uint8_t* GetSlotBase(uint32_t slot_id) const;
  SlotHeader* GetSlotHeader(uint32_t slot_id) const;
  uint8_t* GetSlotData(uint32_t slot_id) const;
  size_t GetSlotStride() const;

  std::string shm_name_;
  size_t slot_count_ = 0;
  size_t slot_capacity_ = 0;
  size_t total_shm_size_ = 0;
  int shm_fd_ = -1;
  uint8_t* base_addr_ = nullptr;
  bool is_creator_ = false;
};

}  // namespace common
}  // namespace soeasy

#endif  // SOEASY_SRC_COMMON_SHM_POOL_H_
