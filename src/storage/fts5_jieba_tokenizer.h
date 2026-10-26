#ifndef SOEASY_SRC_STORAGE_FTS5_JIEBA_TOKENIZER_H_
#define SOEASY_SRC_STORAGE_FTS5_JIEBA_TOKENIZER_H_

#include <string>

#include "sqlite3mc_amalgamation.h"

namespace soeasy {
namespace storage {

/**
 * @brief Register custom 'jieba' tokenizer into SQLite FTS5 extension.
 *
 * @param db Open SQLite database handle
 * @param dict_dir Directory containing jieba dictionary files
 * @return int SQLITE_OK on success, or SQLite error code
 */
int RegisterFts5JiebaTokenizer(sqlite3* db, const std::string& dict_dir);

}  // namespace storage
}  // namespace soeasy

#endif  // SOEASY_SRC_STORAGE_FTS5_JIEBA_TOKENIZER_H_
