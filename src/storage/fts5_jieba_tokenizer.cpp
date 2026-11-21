#include "fts5_jieba_tokenizer.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "storage/dat_jieba.h"
#include "utf8.h"

namespace soeasy {
namespace storage {

namespace {

std::string NormalizeDictDir(const std::string &dict_dir) {
  std::string norm = dict_dir;
  std::replace(norm.begin(), norm.end(), '\\', '/');
  while (norm.size() > 1 && norm.back() == '/') {
    norm.pop_back();
  }
  return norm;
}

struct SharedJiebaRegistry {
  std::mutex mutex;
  std::unordered_map<std::string, std::weak_ptr<DatJieba>> by_dir;
};

SharedJiebaRegistry &GetSharedJiebaRegistry() {
  static SharedJiebaRegistry registry;
  return registry;
}

std::shared_ptr<DatJieba> AcquireSharedJieba(const std::string &dict_dir) {
  const std::string key = NormalizeDictDir(dict_dir);
  auto &registry = GetSharedJiebaRegistry();
  std::lock_guard<std::mutex> lock(registry.mutex);

  for (auto it = registry.by_dir.begin(); it != registry.by_dir.end();) {
    if (it->second.expired()) {
      it = registry.by_dir.erase(it);
    } else {
      ++it;
    }
  }

  auto it = registry.by_dir.find(key);
  if (it != registry.by_dir.end()) {
    if (auto existing = it->second.lock()) {
      return existing;
    }
  }

  auto dj = std::make_shared<DatJieba>();
  if (!dj->Init(dict_dir)) {
    return nullptr;
  }
  dj->Freeze();
  registry.by_dir[key] = dj;
  return dj;
}

struct JiebaContext {
  std::string dict_dir;
  mutable std::mutex mutex;
  mutable std::shared_ptr<DatJieba> jieba;

  std::shared_ptr<DatJieba> GetJieba() const {
    std::lock_guard<std::mutex> lock(mutex);
    if (!jieba) {
      jieba = AcquireSharedJieba(dict_dir);
    }
    return jieba;
  }
};

struct JiebaTokenizerInstance {
  JiebaContext *ctx{nullptr};
};

int xCreate(void *user_data, const char **argv, int argc, Fts5Tokenizer **out_tokenizer) {
  auto *ctx = static_cast<JiebaContext *>(user_data);
  if (!ctx) {
    return SQLITE_ERROR;
  }

  auto *instance = new (std::nothrow) JiebaTokenizerInstance();
  if (!instance) {
    return SQLITE_NOMEM;
  }
  instance->ctx = ctx;

  *out_tokenizer = reinterpret_cast<Fts5Tokenizer *>(instance);
  return SQLITE_OK;
}

void xDelete(Fts5Tokenizer *tokenizer) {
  if (tokenizer) {
    auto *instance = reinterpret_cast<JiebaTokenizerInstance *>(tokenizer);
    delete instance;
  }
}

constexpr size_t kTargetChunkBytes = 32 * 1024;
constexpr size_t kMinChunkBytes = 16 * 1024;

bool IsCjkBoundaryPunct3(const char *p) {
  // UTF-8 3-byte Chinese sentence/clause punctuation: 。(E3 80 82) 、(E3 80 81) ；(EF BC 9B) ！(EF BC 81) ？(EF BC 9F)
  const auto b0 = static_cast<unsigned char>(p[0]);
  const auto b1 = static_cast<unsigned char>(p[1]);
  const auto b2 = static_cast<unsigned char>(p[2]);
  if (b0 == 0xE3 && b1 == 0x80 && (b2 == 0x81 || b2 == 0x82)) {
    return true;
  }
  if (b0 == 0xEF && b1 == 0xBC && (b2 == 0x81 || b2 == 0x9B || b2 == 0x9F)) {
    return true;
  }
  return false;
}

bool IsTier1Boundary(char c) { return c == '\n' || c == '\r'; }

bool IsTier2AsciiBoundary(char c) { return c == ' ' || c == '\t' || c == ',' || c == ';'; }

size_t FindBoundaryBackward(const char *data, size_t begin_exclusive, size_t end_inclusive) {
  // Tier 1: Line / paragraph boundary (\n or \r)
  for (size_t i = end_inclusive; i > begin_exclusive; --i) {
    if (IsTier1Boundary(data[i - 1])) {
      return i;
    }
  }

  // Tier 2: Whitespace or ASCII / CJK clause punctuation boundary
  for (size_t i = end_inclusive; i > begin_exclusive; --i) {
    if (IsTier2AsciiBoundary(data[i - 1])) {
      return i;
    }
    if (i >= begin_exclusive + 3 && IsCjkBoundaryPunct3(data + i - 3)) {
      return i;
    }
  }
  return 0;
}

size_t FindChunkEnd(const char *data, size_t len, size_t start, bool *out_discard) {
  *out_discard = false;
  if (len - start <= kTargetChunkBytes) {
    return len;
  }
  const size_t target = start + kTargetChunkBytes;
  const size_t min_pos = start + kMinChunkBytes;

  // Prefer splitting within the [16KB, 32KB] window
  const size_t split = FindBoundaryBackward(data, min_pos, target);
  if (split != 0) {
    return split;
  }

  // No boundary in (16KB, 32KB]: an abnormal >16KB delimiter-free run crosses [min_pos, target].
  // If valid text precedes this abnormal run in (start, min_pos], flush the valid prefix first.
  const size_t prefix_split = FindBoundaryBackward(data, start, min_pos);
  if (prefix_split != 0) {
    return prefix_split;
  }

  // [start, target] contains no boundary at all: discard this entire delimiter-free run.
  *out_discard = true;
  size_t end = target;
  while (end < len) {
    const char c = data[end];
    if (IsTier1Boundary(c) || IsTier2AsciiBoundary(c)) {
      return end + 1;
    }
    if (end + 3 <= len && IsCjkBoundaryPunct3(data + end)) {
      return end + 3;
    }
    ++end;
  }
  return len;
}

int EmitWordsForChunk(const std::vector<cppjieba::Word> &words, size_t chunk_base_offset, int flags, void *ctx,
                      int (*token_cb)(void *ctx, int tflags, const char *token, int token_len, int start_offset,
                                      int end_offset),
                      std::string *lower_token) {
  for (const auto &w : words) {
    if (w.word.empty()) {
      continue;
    }

    // Skip tokens that are pure whitespace
    bool all_whitespace = true;
    for (char c : w.word) {
      if (!std::isspace(static_cast<unsigned char>(c))) {
        all_whitespace = false;
        break;
      }
    }
    if (all_whitespace) {
      continue;
    }

    // Fold ASCII uppercase to lowercase for case-insensitive matching
    lower_token->assign(w.word);
    std::transform(lower_token->begin(), lower_token->end(), lower_token->begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const size_t token_start = chunk_base_offset + w.offset;
    const int start_pos = static_cast<int>(token_start);
    const int end_pos = static_cast<int>(token_start + w.word.size());

    // 1. Emit primary word token
    int rc = token_cb(ctx, 0, lower_token->data(), static_cast<int>(lower_token->size()), start_pos, end_pos);
    if (rc != SQLITE_OK) {
      return rc;
    }

    // 2. Colocated sub-character emission for multi-character CJK words:
    // Enables single-character queries to match compound words in documents.
    // CRITICAL: NEVER emit colocated sub-characters during QUERY tokenization
    // (flags & FTS5_TOKENIZE_QUERY), because SQLite FTS5 treats colocated tokens
    // in a query as OR alternatives (which would turn "测试" into "测试 OR 测 OR 试"
    // and falsely match documents containing "检测" or "考试")!
    if (!(flags & FTS5_TOKENIZE_QUERY)) {
      bool has_cjk = false;
      for (char c : *lower_token) {
        if (static_cast<unsigned char>(c) >= 0x80) {
          has_cjk = true;
          break;
        }
      }

      std::string::const_iterator it = lower_token->begin();
      std::string::const_iterator end = lower_token->end();
      size_t char_count = 0;
      try {
        char_count = utf8::distance(it, end);
      } catch (...) {
        char_count = 0;
      }

      if (has_cjk && char_count > 1) {
        size_t byte_offset = token_start;
        while (it != end) {
          std::string::const_iterator next = it;
          try {
            utf8::next(next, end);
          } catch (...) {
            break;
          }
          size_t char_len = next - it;
          rc = token_cb(ctx, FTS5_TOKEN_COLOCATED, &(*it), static_cast<int>(char_len), static_cast<int>(byte_offset),
                        static_cast<int>(byte_offset + char_len));
          if (rc != SQLITE_OK) {
            return rc;
          }

          byte_offset += char_len;
          it = next;
        }
      }
    }
  }
  return SQLITE_OK;
}

int xTokenize(Fts5Tokenizer *tokenizer, void *ctx, int flags, const char *text, int text_len,
              int (*token_cb)(void *ctx, int tflags, const char *token, int token_len, int start_offset,
                              int end_offset)) {
  if (!tokenizer || !text || text_len <= 0) {
    return SQLITE_OK;
  }

  auto *instance = reinterpret_cast<JiebaTokenizerInstance *>(tokenizer);
  if (!instance->ctx) {
    return SQLITE_ERROR;
  }

  auto jieba = instance->ctx->GetJieba();
  if (!jieba) {
    return SQLITE_ERROR;
  }

  struct Scratch {
    std::string sentence;
    std::vector<cppjieba::Word> words;
    std::string lower_token;
  };
  thread_local Scratch tls;

  const size_t src_len = static_cast<size_t>(text_len);
  int rc = SQLITE_OK;
  size_t pos = 0;
  while (pos < src_len) {
    bool discard = false;
    const size_t end = FindChunkEnd(text, src_len, pos, &discard);
    if (discard || !utf8::is_valid(text + pos, text + end)) {
      pos = end;
      continue;
    }

    tls.sentence.assign(text + pos, end - pos);
    tls.words.clear();

    // Use precise Cut mode for both query and indexing to maintain accurate token positions and phrase distances
    jieba->Cut(tls.sentence, tls.words, true);

    rc = EmitWordsForChunk(tls.words, pos, flags, ctx, token_cb, &tls.lower_token);
    if (rc != SQLITE_OK) {
      break;
    }
    pos = end;
  }

  if (tls.sentence.capacity() > 128 * 1024) std::string().swap(tls.sentence);
  if (tls.words.capacity() > 16384) std::vector<cppjieba::Word>().swap(tls.words);
  return rc;
}

void xDestroyContext(void *user_data) {
  if (user_data) {
    auto *ctx = static_cast<JiebaContext *>(user_data);
    delete ctx;
  }
}

}  // namespace

int RegisterFts5JiebaTokenizer(sqlite3 *db, const std::string &dict_dir) {
  if (!db) {
    return SQLITE_MISUSE;
  }

  fts5_api *api_ptr = nullptr;
  sqlite3_stmt *stmt = nullptr;
  int rc = sqlite3_prepare_v2(db, "SELECT fts5(?1)", -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return rc;
  }

  rc = sqlite3_bind_pointer(stmt, 1, &api_ptr, "fts5_api_ptr", nullptr);
  if (rc != SQLITE_OK) {
    sqlite3_finalize(stmt);
    return rc;
  }

  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (!api_ptr) {
    return SQLITE_ERROR;
  }

  auto *ctx = new (std::nothrow) JiebaContext();
  if (!ctx) {
    return SQLITE_NOMEM;
  }
  ctx->dict_dir = dict_dir;

  fts5_tokenizer tokenizer;
  tokenizer.xCreate = xCreate;
  tokenizer.xDelete = xDelete;
  tokenizer.xTokenize = xTokenize;

  rc = api_ptr->xCreateTokenizer(api_ptr, "jieba", ctx, &tokenizer, xDestroyContext);
  return rc;
}

}  // namespace storage
}  // namespace soeasy
