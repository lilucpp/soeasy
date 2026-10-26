#include "fts5_jieba_tokenizer.h"

#include <cctype>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cppjieba/Jieba.hpp"
#include "utf8.h"

namespace soeasy {
namespace storage {

namespace {

struct JiebaContext {
  std::shared_ptr<cppjieba::Jieba> jieba;
};

struct JiebaTokenizerInstance {
  std::shared_ptr<cppjieba::Jieba> jieba;
};

int xCreate(void* user_data, const char** argv, int argc,
            Fts5Tokenizer** out_tokenizer) {
  auto* ctx = static_cast<JiebaContext*>(user_data);
  if (!ctx || !ctx->jieba) {
    return SQLITE_ERROR;
  }

  auto* instance = new (std::nothrow) JiebaTokenizerInstance();
  if (!instance) {
    return SQLITE_NOMEM;
  }
  instance->jieba = ctx->jieba;

  *out_tokenizer = reinterpret_cast<Fts5Tokenizer*>(instance);
  return SQLITE_OK;
}

void xDelete(Fts5Tokenizer* tokenizer) {
  if (tokenizer) {
    auto* instance = reinterpret_cast<JiebaTokenizerInstance*>(tokenizer);
    delete instance;
  }
}

int xTokenize(Fts5Tokenizer* tokenizer, void* ctx, int flags,
              const char* text, int text_len,
              int (*token_cb)(void* ctx, int tflags, const char* token,
                              int token_len, int start_offset, int end_offset)) {
  if (!tokenizer || !text || text_len <= 0) {
    return SQLITE_OK;
  }

  auto* instance = reinterpret_cast<JiebaTokenizerInstance*>(tokenizer);
  if (!instance->jieba) {
    return SQLITE_ERROR;
  }

  std::string sentence(text, text_len);

  // Ensure valid UTF-8 sequence
  if (!utf8::is_valid(sentence.begin(), sentence.end())) {
    std::string cleaned;
    utf8::replace_invalid(sentence.begin(), sentence.end(),
                          std::back_inserter(cleaned));
    sentence = std::move(cleaned);
  }

  std::vector<cppjieba::Word> words;

  // Use CutForSearch for indexing and Cut for query matching
  if (flags & FTS5_TOKENIZE_QUERY) {
    instance->jieba->Cut(sentence, words, true);
  } else {
    instance->jieba->CutForSearch(sentence, words, true);
  }

  for (const auto& w : words) {
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
    std::string lower_token = w.word;
    for (char& c : lower_token) {
      if (c >= 'A' && c <= 'Z') {
        c = static_cast<char>(c + 32);
      }
    }

    int start_pos = static_cast<int>(w.offset);
    int end_pos = static_cast<int>(w.offset + w.word.size());

    // 1. Emit primary word token
    int rc = token_cb(ctx, 0, lower_token.data(),
                      static_cast<int>(lower_token.size()), start_pos, end_pos);
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
      for (char c : lower_token) {
        if (static_cast<unsigned char>(c) >= 0x80) {
          has_cjk = true;
          break;
        }
      }

      std::string::const_iterator it = lower_token.begin();
      std::string::const_iterator end = lower_token.end();
      size_t char_count = 0;
      try {
        char_count = utf8::distance(it, end);
      } catch (...) {
        char_count = 0;
      }

      if (has_cjk && char_count > 1) {
        size_t byte_offset = w.offset;
        while (it != end) {
          std::string::const_iterator next = it;
          try {
            utf8::next(next, end);
          } catch (...) {
            break;
          }
          size_t char_len = next - it;
          std::string single_char(it, next);

          rc = token_cb(ctx, FTS5_TOKEN_COLOCATED, single_char.data(),
                        static_cast<int>(single_char.size()),
                        static_cast<int>(byte_offset),
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

void xDestroyContext(void* user_data) {
  if (user_data) {
    auto* ctx = static_cast<JiebaContext*>(user_data);
    delete ctx;
  }
}

}  // namespace

int RegisterFts5JiebaTokenizer(sqlite3* db, const std::string& dict_dir) {
  if (!db) {
    return SQLITE_MISUSE;
  }

  fts5_api* api_ptr = nullptr;
  sqlite3_stmt* stmt = nullptr;
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

  std::string dict_path = dict_dir + "/jieba.dict.utf8";
  std::string model_path = dict_dir + "/hmm_model.utf8";
  std::string user_dict_path = dict_dir + "/user.dict.utf8";
  std::string idf_path = dict_dir + "/idf.utf8";
  std::string stop_words_path = dict_dir + "/stop_words.utf8";

  auto jieba_ptr = std::make_shared<cppjieba::Jieba>(
      dict_path, model_path, user_dict_path, idf_path, stop_words_path);

  auto* ctx = new JiebaContext{jieba_ptr};

  fts5_tokenizer tokenizer;
  tokenizer.xCreate = xCreate;
  tokenizer.xDelete = xDelete;
  tokenizer.xTokenize = xTokenize;

  rc = api_ptr->xCreateTokenizer(api_ptr, "jieba", ctx, &tokenizer,
                                 xDestroyContext);
  return rc;
}

}  // namespace storage
}  // namespace soeasy
