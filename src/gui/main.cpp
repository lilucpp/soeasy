// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QStandardPaths>

#include <iostream>
#include <memory>
#include <string>

#include "common/logger.h"
#include "gui/main_window.h"
#include "service/index_config.h"
#include "service/index_pipeline.h"
#include "service/worker_guard.h"
#include "storage/storage_engine.h"

int main(int argc, char* argv[]) {
  // High DPI scaling attributes for Qt5
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
  QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
      Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif

  QApplication app(argc, argv);
  app.setApplicationName("SoEasy");
  app.setApplicationDisplayName("SoEasy - 让电脑没有难搜的文件");
  app.setApplicationVersion("1.0.0");

  QCommandLineParser parser;
  parser.setApplicationDescription("SoEasy Desktop Search Engine");
  parser.addHelpOption();
  parser.addVersionOption();

  QCommandLineOption db_option(
      QStringList() << "d" << "db",
      "Path to encrypted SQLite3MC database file.",
      "path");
  parser.addOption(db_option);

  QCommandLineOption worker_option(
      QStringList() << "w" << "worker",
      "Path to soeasy-worker binary.",
      "path");
  parser.addOption(worker_option);

  QCommandLineOption dict_option(
      QStringList() << "dict",
      "Path to Jieba dictionary directory.",
      "path");
  parser.addOption(dict_option);

  parser.process(app);

  // Determine data storage directory: ~/.local/share/soeasy or ~/.config/soeasy
  QString app_data_dir =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  if (app_data_dir.isEmpty()) {
    app_data_dir = QDir::homePath() + "/.soeasy";
  }
  QDir().mkpath(app_data_dir);

  // Initialize spdlog rotating file logger (3 files, 10MB each)
  QString log_path = app_data_dir + "/logs/soeasy.log";
  soeasy::common::Logger::Init(log_path.toStdString(), 10 * 1024 * 1024, 3, true);
  LOG_INFO("========================================================");
  LOG_INFO("   SoEasy Desktop Search Engine Starting (v1.0.0)");
  LOG_INFO("   AppData Directory: {}", app_data_dir.toStdString());
  LOG_INFO("   Rolling Log File:  {} (3 files x 10MB)", log_path.toStdString());
  LOG_INFO("========================================================");

  QString db_path = parser.value(db_option);
  if (db_path.isEmpty()) {
    db_path = app_data_dir + "/soeasy_index.db";
  }

  // Locate worker binary
  QString worker_bin = parser.value(worker_option);
  if (worker_bin.isEmpty()) {
    QString candidate1 =
        QCoreApplication::applicationDirPath() + "/soeasy-worker";
    QString candidate2 =
        QCoreApplication::applicationDirPath() + "/../bin/soeasy-worker";
    QString candidate3 = "./soeasy-worker";

    if (QFileInfo::exists(candidate1)) {
      worker_bin = candidate1;
    } else if (QFileInfo::exists(candidate2)) {
      worker_bin = candidate2;
    } else {
      worker_bin = candidate3;
    }
  }

  // Locate Jieba dictionary directory
  QString dict_dir = parser.value(dict_option);
  if (dict_dir.isEmpty()) {
    QStringList dict_candidates = {
        QCoreApplication::applicationDirPath() + "/../third_party/cppjieba/dict",
        QCoreApplication::applicationDirPath() + "/dict",
        "/home/peanut/project/soeasy/third_party/cppjieba/dict",
        "/usr/local/share/soeasy/dict"
    };
    for (const auto& candidate : dict_candidates) {
      if (QFileInfo::exists(candidate + "/jieba.dict.utf8")) {
        dict_dir = candidate;
        break;
      }
    }
    if (dict_dir.isEmpty()) {
      dict_dir = "/home/peanut/project/soeasy/third_party/cppjieba/dict";
    }
  }

  // 1. Initialize Storage Engine (SQLite3MC with AES-128 page encryption)
  auto storage_engine = std::make_shared<soeasy::storage::StorageEngine>();
  const std::string kEncryptionKey = "soeasy_master_page_key_2026";
  if (!storage_engine->Open(db_path.toStdString(), kEncryptionKey, dict_dir.toStdString())) {
    QMessageBox::critical(
        nullptr, "SoEasy - 启动错误",
        QString("无法打开加密索引数据库:\n%1\n请检查权限或磁盘空间。")
            .arg(db_path));
    return 1;
  }

  // 2. Initialize Index Config from persistent JSON file
  auto config = std::make_shared<soeasy::service::IndexConfig>();
  QString config_path = app_data_dir + "/config.json";
  config->set_config_path(config_path.toStdString());

  bool config_loaded = config->LoadFromFile(config_path.toStdString());
  if (!config_loaded || config->GetIndexedDirectories().empty()) {
    QString docs =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString desktop =
        QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (!docs.isEmpty() && QDir(docs).exists()) {
      config->AddIndexedDirectory(docs.toStdString());
    }
    if (!desktop.isEmpty() && QDir(desktop).exists()) {
      config->AddIndexedDirectory(desktop.toStdString());
    }
    config->SaveToFile(config_path.toStdString());
    LOG_INFO("[Config] Created initial persistent config at: {}", config_path.toStdString());
  } else {
    LOG_INFO("[Config] Successfully loaded persistent config from: {}", config_path.toStdString());
  }

  // 3. Initialize Worker Guard with configured worker count and 15s base timeout
  auto worker_guard = std::make_shared<soeasy::service::WorkerGuard>();
  if (!worker_guard->Init(worker_bin.toStdString(), config->worker_count(),
                          soeasy::service::WorkerGuard::kDefaultTimeoutMs)) {
    std::cerr << "[Warning] Worker guard failed to spawn workers immediately. "
              << "Workers will be lazily initialized or retried." << std::endl;
  }

  // 4. Initialize Index Pipeline
  auto pipeline = std::make_shared<soeasy::service::IndexPipeline>(
      config, worker_guard, storage_engine);
  if (!pipeline->Init()) {
    QMessageBox::critical(nullptr, "SoEasy - 启动错误",
                          "初始化索引流水线与文件监视引擎失败。");
    return 1;
  }

  // Scan initial configured directories and start live watch
  for (const auto& dir : config->GetIndexedDirectories()) {
    pipeline->AddAndScanDirectory(dir, true);
  }
  pipeline->StartLiveWatch();

  // 5. Construct and show MainWindow
  soeasy::gui::MainWindow main_window(pipeline);
  main_window.show();

  int exit_code = app.exec();

  // Clean shutdown
  pipeline->StopLiveWatch();
  worker_guard.reset();
  storage_engine->Close();

  return exit_code;
}
