#ifndef SOEASY_SRC_STORAGE_MMAP_FILE_H_
#define SOEASY_SRC_STORAGE_MMAP_FILE_H_

#include <cstddef>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace soeasy {
namespace storage {

class MmapFile {
 public:
  MmapFile() = default;

  ~MmapFile() {
    Close();
  }

  // Disable copy semantics
  MmapFile(const MmapFile &) = delete;
  MmapFile &operator=(const MmapFile &) = delete;

  // Enable move semantics
  MmapFile(MmapFile &&other) noexcept {
    MoveFrom(std::move(other));
  }

  MmapFile &operator=(MmapFile &&other) noexcept {
    if (this != &other) {
      Close();
      MoveFrom(std::move(other));
    }
    return *this;
  }

  bool Open(const std::string &path) {
    Close();

#ifdef _WIN32
    file_handle_ = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_handle_ == INVALID_HANDLE_VALUE) {
      return false;
    }

    LARGE_INTEGER li_size;
    if (!GetFileSizeEx(file_handle_, &li_size) || li_size.QuadPart <= 0) {
      Close();
      return false;
    }
    size_ = static_cast<size_t>(li_size.QuadPart);

    mapping_handle_ = CreateFileMappingA(file_handle_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping_handle_) {
      Close();
      return false;
    }

    data_ = MapViewOfFile(mapping_handle_, FILE_MAP_READ, 0, 0, 0);
    if (!data_) {
      Close();
      return false;
    }
#else
    fd_ = open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
      return false;
    }

    struct stat st;
    if (fstat(fd_, &st) != 0 || st.st_size <= 0) {
      Close();
      return false;
    }
    size_ = static_cast<size_t>(st.st_size);

    data_ = mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd_, 0);
    if (data_ == MAP_FAILED) {
      data_ = nullptr;
      Close();
      return false;
    }

#if defined(MADV_WILLNEED)
    madvise(data_, size_, MADV_WILLNEED);
#endif
#endif

    return true;
  }

  void Close() {
#ifdef _WIN32
    if (data_) {
      UnmapViewOfFile(data_);
      data_ = nullptr;
    }
    if (mapping_handle_) {
      CloseHandle(mapping_handle_);
      mapping_handle_ = nullptr;
    }
    if (file_handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(file_handle_);
      file_handle_ = INVALID_HANDLE_VALUE;
    }
#else
    if (data_ && data_ != MAP_FAILED) {
      munmap(data_, size_);
      data_ = nullptr;
    }
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
#endif
    size_ = 0;
  }

  const void *data() const {
    return data_;
  }

  size_t size() const {
    return size_;
  }

  bool is_open() const {
    return data_ != nullptr;
  }

 private:
  void MoveFrom(MmapFile &&other) noexcept {
    data_ = other.data_;
    size_ = other.size_;
    other.data_ = nullptr;
    other.size_ = 0;

#ifdef _WIN32
    file_handle_ = other.file_handle_;
    mapping_handle_ = other.mapping_handle_;
    other.file_handle_ = INVALID_HANDLE_VALUE;
    other.mapping_handle_ = nullptr;
#else
    fd_ = other.fd_;
    other.fd_ = -1;
#endif
  }

  void *data_{nullptr};
  size_t size_{0};

#ifdef _WIN32
  HANDLE file_handle_{INVALID_HANDLE_VALUE};
  HANDLE mapping_handle_{nullptr};
#else
  int fd_{-1};
#endif
};

}  // namespace storage
}  // namespace soeasy

#endif  // SOEASY_SRC_STORAGE_MMAP_FILE_H_
