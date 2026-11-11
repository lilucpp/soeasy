#ifndef SOEASY_SRC_GUI_SEARCH_BAR_H_
#define SOEASY_SRC_GUI_SEARCH_BAR_H_

#include <QComboBox>
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
  explicit SearchBar(QWidget *parent = nullptr);
  ~SearchBar() override = default;

  QString query() const;
  SearchMode mode() const;
  QString path_scope() const { return path_scope_; }

  void SetQuery(const QString &query);
  void SetMode(SearchMode mode);
  void SetPathScope(const QString &path) { path_scope_ = path; }
  void FocusQuery();
  void ClearQuery();
  void RetranslateUi();

 signals:
  void SearchTriggered(const QString &query, int mode, const QString &path_scope);
  void SettingsRequested();
  void NavigateResultsRequested();

 protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

 private slots:
  void OnQueryTextChanged();
  void OnQueryImmediate();

 private:
  void SetupUi();

  QLineEdit *query_edit_ = nullptr;
  QComboBox *mode_combo_ = nullptr;
  QPushButton *btn_settings_ = nullptr;

  QString path_scope_;
  QTimer *debounce_timer_ = nullptr;
};

}  // namespace gui
}  // namespace soeasy

#endif  // SOEASY_SRC_GUI_SEARCH_BAR_H_
