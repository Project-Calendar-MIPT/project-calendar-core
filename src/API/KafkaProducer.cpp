#include "API/KafkaProducer.h"

#include <trantor/utils/Logger.h>

KafkaProducer& KafkaProducer::instance() {
  static KafkaProducer inst;
  return inst;
}

bool KafkaProducer::init(const std::string& brokers) {
  if (brokers.empty()) {
    LOG_WARN << "KafkaProducer: no broker address, Kafka disabled";
    return false;
  }

  std::string errstr;
  auto conf = std::unique_ptr<RdKafka::Conf>(
      RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));

  if (conf->set("bootstrap.servers", brokers, errstr) !=
      RdKafka::Conf::CONF_OK) {
    LOG_ERROR << "KafkaProducer: bootstrap.servers error: " << errstr;
    return false;
  }
  // Fire-and-forget: don't wait for acks on every message
  if (conf->set("acks", "1", errstr) != RdKafka::Conf::CONF_OK) {
    LOG_WARN << "KafkaProducer: acks config warning: " << errstr;
  }

  producer_.reset(RdKafka::Producer::create(conf.get(), errstr));
  if (!producer_) {
    LOG_ERROR << "KafkaProducer: failed to create producer: " << errstr;
    return false;
  }

  LOG_INFO << "KafkaProducer: connected to " << brokers;
  return true;
}

void KafkaProducer::produce(const std::string& topic, const std::string& key,
                            const std::string& message) {
  if (!producer_) {
    LOG_WARN << "KafkaProducer: not initialised, skipping message to " << topic;
    return;
  }

  RdKafka::ErrorCode err = producer_->produce(
      topic, RdKafka::Topic::PARTITION_UA, RdKafka::Producer::RK_MSG_COPY,
      const_cast<char*>(message.data()), message.size(),
      key.data(), key.size(),
      0, nullptr, nullptr);

  if (err != RdKafka::ERR_NO_ERROR) {
    LOG_ERROR << "KafkaProducer: produce failed for topic " << topic << ": "
              << RdKafka::err2str(err);
  } else {
    producer_->poll(0);
  }
}
