#pragma once

#include <librdkafka/rdkafkacpp.h>
#include <memory>
#include <string>

class KafkaProducer {
 public:
  static KafkaProducer& instance();

  // Returns false if Kafka is unavailable (logs warning, doesn't throw).
  bool init(const std::string& brokers);

  void produce(const std::string& topic, const std::string& key,
               const std::string& message);

  bool isReady() const { return producer_ != nullptr; }

 private:
  KafkaProducer() = default;
  std::unique_ptr<RdKafka::Producer> producer_;
};
