#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <trantor/utils/Logger.h>
#include <trantor/utils/AsyncFileLogger.h>
#include <cstdlib>
#include <string>

#include "API/KafkaProducer.h"

int main() {
  trantor::Logger::setLogLevel(trantor::Logger::kInfo);

  // Write logs to file when LOG_PATH env var is set (e.g. /var/log/app)
  static trantor::AsyncFileLogger asyncFileLogger;
  const char* logPath = std::getenv("LOG_PATH");
  if (logPath && logPath[0] != '\0') {
    asyncFileLogger.setFileName(std::string(logPath) + "/project-calendar");
    asyncFileLogger.startLogging();
    trantor::Logger::setOutputFunction(
        [](const char* msg, const uint64_t len) {
          asyncFileLogger.output(msg, len);
        },
        []() { asyncFileLogger.flush(); });
  }
  
  // Get database configuration from environment
  const char* dbHost = std::getenv("DB_HOST");
  const char* dbPort = std::getenv("DB_PORT");
  const char* dbName = std::getenv("DB_NAME");
  const char* dbUser = std::getenv("DB_USER");
  const char* dbPassword = std::getenv("DB_PASSWORD");

  // Set defaults if environment variables are not set
  std::string host = dbHost ? dbHost : "localhost";
  int port = dbPort ? std::stoi(dbPort) : 5432;
  std::string database = dbName ? dbName : "project_calendar";
  std::string user = dbUser ? dbUser : "pc_admin";
  std::string password = dbPassword ? dbPassword : "pc_password";

  LOG_INFO << "Connecting to database: " << host << ":" << port << "/" << database;

  // Configure database with createDbClient (13 params)
  drogon::app().createDbClient(
    "postgresql",  // dbType
    host,          // host
    static_cast<unsigned short>(port),  // port
    database,      // databaseName
    user,          // userName
    password,      // password
    5,             // connectionNum
    "",            // filename (empty for postgres)
    "default",     // name
    false,         // isFast
    "",            // characterSet
    -1.0,          // timeout (no timeout)
    false          // autoBatch - must be false so sync SELECT after trans commit works
  );

  // Configure HTTP server
  drogon::app()
      .setThreadNum(4)
      .addListener("0.0.0.0", 8080)
      .setLogLevel(trantor::Logger::kInfo);

  // Initialise Kafka producer (optional — degrades gracefully if unavailable)
  {
    const char* brokers = std::getenv("KAFKA_BOOTSTRAP_SERVERS");
    KafkaProducer::instance().init(brokers ? brokers : "");
  }

  LOG_INFO << "Server starting on http://0.0.0.0:8080";

  // Run the application
  drogon::app().run();
  
  return 0;
}
