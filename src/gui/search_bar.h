#ifndef SOEASY_SRC_GUI_SEARCH_BAR_H_
#define SOEASY_SRC_GUI_SEARCH_BAR_H_

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QString>
#include <QTimer>
#include <QWidget>

namespace soeasy {
namespace gui {

enum SearchMode {
  kModeFulltext = 0,
  kModeFilename = 1,
};

class SearchBar : public QWidget {
  Q_OBJECT

 public:
  explicit SearchBar(QWidget* parent = nullptr);
  ~SearchBar() override = default;

  QString query() const;
  SearchMode mode() const;
  QString path_scope() const;

  void SetQuery(const QString& query);
  void SetMode(SearchMode mode);
  void SetPathScope(const QString& path);
  void RetranslateUi();

 signals:
  void SearchTriggered(const QString& query, int mode, const QString& path_scope);
  void SettingsRequested();
  void PreviewToggleRequested();

 private slots:
  void OnBrowseScope();
  void OnClearScope();
  void OnQueryTextChanged();
  void OnQueryImmediate();

 private:
  void SetupUi();

  QLineEdit* query_edit_ = nullptr;
  QComboBox* mode_combo_ = nullptr;
  QLineEdit* scope_edit_ = nullptr;
  QLabel* label_scope_ = nullptr;
  QPushButton* btn_scope_browse_ = nullptr;
  QPushButton* btn_scope_clear_ = nullptr;
  QPushButton* btn_settings_ = nullptr;
  QPushButton* btn_toggle_preview_ = nullptr;

  QTimer* debounce_timer_ = nullptr;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_SEARCH_BAR_H_
