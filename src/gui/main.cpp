// Copyright 2026 SoEasy Team. All rights reserved.
// Use of this source code is governed by an Apache-2.0 license.

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMessageBox>
#include <QStandardPaths>
#include <iostream>
#include <memory>
#include <string>

#include "common/logger.h"
#include "common/machine_key_provider.h"
#include "common/system_env.h"
#include "gui/i18n.h"
#include "gui/icon_provider.h"
#include "gui/main_window.h"
#include "service/index_config.h"
#include "service/index_pipeline.h"
#include "service/worker_guard.h"
#include "storage/storage_engine.h"

int main(int argc, char *argv[]) {
#ifdef _WIN32
  // Enable Per-Monitor DPI Awareness V2 on Windows 10/11
  // This completely disables blurry OS bitmap scaling (DPI virtualization)
  HMODULE user32 = LoadLibraryA("user32.dll");
  if (user32) {
    typedef BOOL(WINAPI * SetProcessDpiAwarenessContextFunc)(void *);
    auto set_context =
        reinterpret_cast<SetProcessDpiAwarenessContextFunc>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (set_context) {
      // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = ((DPI_AWARENESS_CONTEXT)-4)
      set_context(reinterpret_cast<void *>(-4));
    } else {
      HMODULE shcore = LoadLibraryA("shcore.dll");
      if (shcore) {
        typedef HRESULT(WINAPI * SetProcessDpiAwarenessFunc)(int);
        auto set_awareness =
            reinterpret_cast<SetProcessDpiAwarenessFunc>(GetProcAddress(shcore, "SetProcessDpiAwareness"));
        if (set_awareness) {
          set_awareness(2);  // PROCESS_PER_MONITOR_DPI_AWARE
        }
        FreeLibrary(shcore);
      }
    }
    FreeLibrary(user32);
  }
#endif

  // High DPI scaling attributes for Qt5
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
  QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
  QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif

  QApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  app.setApplicationName("SoEasy");
  app.setApplicationDisplayName("SoEasy - 让电脑没有难搜的文件");
  app.setApplicationVersion("1.0.0");
  app.setWindowIcon(soeasy::gui::IconProvider::GetAppIcon());

  // Configure modern, anti-aliased sans-serif fonts across platforms
  QFont app_font;
#ifdef _WIN32
  app_font.setFamily("Microsoft YaHei UI");
  app_font.setPointSize(9);
#elif defined(__APPLE__)
  app_font.setFamily(".AppleSystemUIFont");
  app_font.setPointSize(10);
#else
  app_font.setFamily("Noto Sans CJK SC");
  app_font.setPointSize(10);
#endif
  app_font.setWeight(QFont::Normal);
  app_font.setStyleStrategy(QFont::PreferAntialias);
  app.setFont(app_font);

  QCommandLineParser parser;
  parser.setApplicationDescription("SoEasy Desktop Search Engine");
  parser.addHelpOption();
  parser.addVersionOption();

  QCommandLineOption db_option(QStringList() << "d" << "db", "Path to encrypted SQLite3MC database file.", "path");
  parser.addOption(db_option);

  QCommandLineOption worker_option(QStringList() << "w" << "worker", "Path to soeasy-worker binary.", "path");
  parser.addOption(worker_option);

  QCommandLineOption dict_option(QStringList() << "dict", "Path to Jieba dictionary directory.", "path");
  parser.addOption(dict_option);

  QCommandLineOption log_level_option(QStringList() << "log-level",
                                      "Set logging level (trace, debug, info, warn, error).", "level");
  parser.addOption(log_level_option);

  QCommandLineOption log_console_option(QStringList() << "log-console", "Mirror logs to console stdout (debugging).");
  parser.addOption(log_console_option);

  QCommandLineOption no_log_console_option(QStringList() << "no-log-console",
                                           "Disable console stdout logging for maximum performance.");
  parser.addOption(no_log_console_option);

  QCommandLineOption log_sync_option(QStringList() << "log-sync",
                                     "Enable synchronous log flushing (flush immediately on each write).");
  parser.addOption(log_sync_option);

  QCommandLineOption debug_option(QStringList() << "debug",
                                  "Enable debug mode (equivalent to --log-level=debug --log-console --log-sync).");
  parser.addOption(debug_option);

  QCommandLineOption dt_log_option(QStringList() << "doctotext-logging", "Enable DocToText verbose logging to file.");
  parser.addOption(dt_log_option);

  QCommandLineOption no_dt_log_option(QStringList() << "no-doctotext-logging", "Disable DocToText verbose logging.");
  parser.addOption(no_dt_log_option);

  QCommandLineOption dt_log_path_option(QStringList() << "doctotext-log-path",
                                        "Set path for DocToText verbose log file.", "path");
  parser.addOption(dt_log_path_option);

  parser.process(app);

  // Determine data storage directory: ~/.local/share/soeasy or ~/.config/soeasy
  QString app_data_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  if (app_data_dir.isEmpty()) {
    app_data_dir = QDir::homePath() + "/.soeasy";
  }
  QDir().mkpath(app_data_dir);

  // Single-instance enforcement: ensure only one GUI process is active per user/profile
  const QString instance_server_name =
      "soeasy_gui_" + QString(QCryptographicHash::hash(app_data_dir.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));

  QLocalSocket existing_socket;
  existing_socket.connectToServer(instance_server_name);
  if (existing_socket.waitForConnected(500)) {
    // Another GUI instance is already running! Signal it to bring its window to front and exit.
    existing_socket.write("ACTIVATE\n");
    existing_socket.waitForBytesWritten(1000);
    existing_socket.disconnectFromServer();
    std::cout << "[SoEasy] 另一个 GUI 进程实例正在运行中，已激活已有窗口。" << std::endl;
    return 0;
  }

  // No active instance; claim the local server
  QLocalServer::removeServer(instance_server_name);
  auto *instance_server = new QLocalServer(&app);
  if (!instance_server->listen(instance_server_name)) {
    LOG_WARN("[Main] Failed to start single-instance listener on {}: {}",
             instance_server_name.toStdString(), instance_server->errorString().toStdString());
  }

  // Clean orphan spill temporary files left by previous abnormal exits or crashes
  soeasy::service::WorkerGuard::CleanupOrphanSpillFiles();

  // 2. Initialize Index Config from persistent JSON file
  auto config = std::make_shared<soeasy::service::IndexConfig>();
  QString config_path = app_data_dir + "/config.json";
  config->set_config_path(config_path.toStdString());

  bool config_loaded = config->LoadFromFile(config_path.toStdString());
  if (!config_loaded || config->GetIndexedDirectories().empty()) {
    QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    if (!docs.isEmpty() && QDir(docs).exists()) {
      config->AddIndexedDirectory(docs.toStdString());
    }
    if (!desktop.isEmpty() && QDir(desktop).exists()) {
      config->AddIndexedDirectory(desktop.toStdString());
    }
    config->SaveToFile(config_path.toStdString());
  }

  // Initialize UI language from configuration
  soeasy::gui::I18n::Instance().SetLanguage(config->ui_language());
  app.setApplicationDisplayName(soeasy::gui::Tr("app_title"));

  // Ensure default doctotext log path is populated if empty
  if (config->doctotext_log_path().empty()) {
    config->set_doctotext_log_path((app_data_dir + "/logs/doctotext.log").toStdString());
  }

  // CLI override for doctotext logging
  if (parser.isSet(dt_log_option)) {
    config->set_doctotext_logging(true);
  } else if (parser.isSet(no_dt_log_option)) {
    config->set_doctotext_logging(false);
  }
  if (parser.isSet(dt_log_path_option)) {
    config->set_doctotext_log_path(parser.value(dt_log_path_option).toStdString());
  }

  // Determine effective logging settings (CLI parameters take precedence over persistent config)
  std::string log_level_str = config->log_level();
  bool enable_console = config->log_console();
  bool enable_sync = config->log_sync();

  if (parser.isSet(debug_option)) {
    log_level_str = "debug";
    enable_console = true;
    enable_sync = true;
  } else {
    if (parser.isSet(log_level_option)) {
      log_level_str = parser.value(log_level_option).toStdString();
    }
    if (parser.isSet(log_console_option)) {
      enable_console = true;
    } else if (parser.isSet(no_log_console_option)) {
      enable_console = false;
    }
    if (parser.isSet(log_sync_option)) {
      enable_sync = true;
    }
  }

  auto level_enum = soeasy::common::Logger::ParseLevel(log_level_str);

  // Initialize spdlog rotating file logger (3 files, 10MB each)
  QString log_path = app_data_dir + "/logs/soeasy.log";
  soeasy::common::Logger::Init(log_path.toStdString(), 10 * 1024 * 1024, 3, enable_console, level_enum);
  soeasy::common::Logger::SetSyncMode(enable_sync);

  LOG_INFO("========================================================");
  LOG_INFO("   SoEasy Desktop Search Engine Starting (v1.0.0)");
  LOG_INFO("   AppData Directory: {}", app_data_dir.toStdString());
  LOG_INFO("   Rolling Log File:  {} (3 files x 10MB)", log_path.toStdString());
  LOG_INFO("   Logging Level:     {} | Console: {} | Sync Flush: {}", soeasy::common::Logger::LevelToString(level_enum),
           enable_console ? "ENABLED" : "DISABLED", enable_sync ? "ENABLED" : "DISABLED");
  LOG_INFO("========================================================");

  // Optimize system environment: Process file descriptors and Inotify watches
  soeasy::common::SystemEnv::OptimizeProcessFileLimits();
  soeasy::common::SystemEnv::CheckAndOptimizeInotifyWatches();

  if (!config_loaded) {
    LOG_INFO("[Config] Created initial persistent config at: {}", config_path.toStdString());
  } else {
    LOG_INFO("[Config] Successfully loaded persistent config from: {}", config_path.toStdString());
  }

  QString db_path = parser.value(db_option);
  if (db_path.isEmpty()) {
    db_path = app_data_dir + "/soeasy_index.db";
  }

  // Locate worker binary
  QString worker_bin = parser.value(worker_option);
  if (worker_bin.isEmpty()) {
#ifdef _WIN32
    QString exe_suffix = ".exe";
#else
    QString exe_suffix = "";
#endif
    QString candidate1 = QCoreApplication::applicationDirPath() + "/soeasy-worker" + exe_suffix;
    QString candidate2 = QCoreApplication::applicationDirPath() + "/../bin/soeasy-worker" + exe_suffix;
    QString candidate3 = "./soeasy-worker" + exe_suffix;

    if (QFileInfo::exists(candidate1)) {
      worker_bin = candidate1;
    } else if (QFileInfo::exists(candidate2)) {
      worker_bin = candidate2;
    } else {
      worker_bin = candidate3;
    }
  }

  // Locate Jieba dictionary directory relative to executable or installed paths
  QString dict_dir = parser.value(dict_option);
  if (dict_dir.isEmpty()) {
    QString app_dir = QCoreApplication::applicationDirPath();
    QStringList dict_candidates = {app_dir + "/dict",
                                   app_dir + "/../dict",
                                   app_dir + "/../third_party/cppjieba/dict",
                                   app_dir + "/../../third_party/cppjieba/dict",
                                   "/usr/local/share/soeasy/dict",
                                   "/usr/share/soeasy/dict"};
    for (const auto &candidate : dict_candidates) {
      if (QFileInfo::exists(candidate + "/jieba_dat.bin") || QFileInfo::exists(candidate + "/jieba.dict.utf8")) {
        dict_dir = candidate;
        break;
      }
    }
  }

  config->set_dict_dir(dict_dir.toStdString());

  // 1. Initialize Storage Engine (SQLite3MC with AES-128 page encryption)
  auto storage_engine = std::make_shared<soeasy::storage::StorageEngine>();
  auto key_result = soeasy::common::MachineKeyProvider::GetOrCreateDatabaseKey(app_data_dir.toStdString());
  if (!key_result.success) {
    QMessageBox::critical(nullptr, soeasy::gui::Tr("err_key_title"),
                          soeasy::gui::Tr("err_key_msg").arg(QString::fromStdString(key_result.error_message)));
    return 1;
  }

  const std::string machine_key = key_result.encryption_key;
  bool db_opened = storage_engine->Open(db_path.toStdString(), machine_key, dict_dir.toStdString());

  // Check for legacy migration: if failed to open with machine_key, but database file exists
  if (!db_opened && QFileInfo::exists(db_path) && QFileInfo(db_path).size() > 0) {
    LOG_INFO("[Startup] Database exists but cannot be decrypted with machine key. Attempting legacy migration...");
    const std::string legacy_key = soeasy::common::MachineKeyProvider::kDefaultLegacyKey;
    if (storage_engine->Open(db_path.toStdString(), legacy_key, dict_dir.toStdString())) {
      LOG_INFO("[Startup] Successfully opened legacy database. Performing atomic migration to machine-bound key...");
      if (storage_engine->Rekey(machine_key)) {
        LOG_INFO("[Startup] Legacy database successfully migrated to machine-bound key!");
        db_opened = true;
      } else {
        LOG_ERROR("[Startup] Failed to rekey legacy database: {}", storage_engine->GetLastError());
      }
    }
  }

  if (!db_opened) {
    QMessageBox::critical(nullptr, soeasy::gui::Tr("err_startup_title"),
                          soeasy::gui::Tr("err_db_open_msg").arg(db_path));
    return 1;
  }

  // 3. Initialize Worker Guard with configured worker count and 15s base timeout
  auto worker_guard = std::make_shared<soeasy::service::WorkerGuard>();
  worker_guard->SetLoggingOptions(soeasy::common::Logger::LevelToString(level_enum), enable_console, enable_sync,
                                  config->doctotext_logging(), config->doctotext_log_path(),
                                  config->enable_embedded_image_ocr());
  if (!worker_guard->Init(worker_bin.toStdString(), config->worker_count(),
                          soeasy::service::WorkerGuard::kDefaultTimeoutMs)) {
    std::cerr << "[Warning] Worker guard failed to spawn workers immediately. "
              << "Workers will be lazily initialized or retried." << std::endl;
  }

  // 4. Initialize Index Pipeline
  auto pipeline = std::make_shared<soeasy::service::IndexPipeline>(config, worker_guard, storage_engine);
  if (!pipeline->Init()) {
    QMessageBox::critical(nullptr, soeasy::gui::Tr("err_startup_title"), soeasy::gui::Tr("err_pipeline_init_msg"));
    return 1;
  }

  // Start live file watcher
  pipeline->StartLiveWatch();

  // 5. Construct and show MainWindow immediately (Instant UI appearance)
  soeasy::gui::MainWindow main_window(pipeline);
  main_window.show();

  // Listen for second-instance wakeups
  QObject::connect(instance_server, &QLocalServer::newConnection, [&main_window, instance_server]() {
    QLocalSocket *client = instance_server->nextPendingConnection();
    if (!client) return;
    QObject::connect(client, &QLocalSocket::readyRead, [&main_window, client]() {
      QByteArray msg = client->readAll();
      if (msg.contains("ACTIVATE")) {
        main_window.ShowAndActivate();
      }
    });
  });

  // Asynchronously trigger initial scan in the background with progress reporting on status bar
  main_window.StartBackgroundInitialIndex();

  int exit_code = app.exec();

  // Clean shutdown
  pipeline->StopLiveWatch();
  worker_guard.reset();
  storage_engine->Close();

  return exit_code;
}
