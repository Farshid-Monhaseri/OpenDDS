#include "dds/DCPS/Service_Participant.h"

#include <ace/Proactor.h>
#include <dds/DCPS/transport/framework/TransportRegistry.h>

#include "StoolC.h"
#include "StoolTypeSupportImpl.h"
#include "BuilderTypeSupportImpl.h"

#include "ListenerFactory.h"

#include "TopicListener.h"
#include "DataReaderListener.h"
#include "DataWriterListener.h"
#include "SubscriberListener.h"
#include "PublisherListener.h"
#include "ParticipantListener.h"
#include "Process.h"

#include "json_2_builder.h"
#include "ActionManager.h"

#include <cmath>
#include <iostream>
#include <fstream>
#include <sstream>
#include <mutex>
#include <thread>
#include <iomanip>
#include <condition_variable>

namespace Stool {

// WorkerTopicListener

class WorkerTopicListener : public Builder::TopicListener {
public:
  void on_inconsistent_topic(DDS::Topic_ptr /*the_topic*/, const DDS::InconsistentTopicStatus& /*status*/) {
    std::unique_lock<std::mutex> lock(mutex_);
    ++inconsistent_count_;
  }

  void set_topic(Builder::Topic& topic) {
    topic_ = &topic;
  }

protected:
  std::mutex mutex_;
  Builder::Topic* topic_{0};
  size_t inconsistent_count_{0};
};

// WorkerDataReaderListener

class WorkerDataReaderListener : public Builder::DataReaderListener {
public:

  WorkerDataReaderListener() {}

  WorkerDataReaderListener(size_t expected) : expected_count_(expected) {}

  void on_requested_deadline_missed(DDS::DataReader_ptr /*reader*/, const DDS::RequestedDeadlineMissedStatus& /*status*/) override {
  }

  void on_requested_incompatible_qos(DDS::DataReader_ptr /*reader*/, const DDS::RequestedIncompatibleQosStatus& /*status*/) override {
  }

  void on_sample_rejected(DDS::DataReader_ptr /*reader*/, const DDS::SampleRejectedStatus& /*status*/) override {
  }

  void on_liveliness_changed(DDS::DataReader_ptr /*reader*/, const DDS::LivelinessChangedStatus& /*status*/) override {
  }

  void on_data_available(DDS::DataReader_ptr reader) override {
    //std::cout << "WorkerDataReaderListener::on_data_available" << std::endl;
    DataDataReader_var data_dr = DataDataReader::_narrow(reader);
    if (data_dr) {
      Data data;
      DDS::SampleInfo si;
      DDS::ReturnCode_t status = data_dr->take_next_sample(data, si);
      if (status == DDS::RETCODE_OK && si.valid_data) {
        const Builder::TimeStamp& now = Builder::get_time();
        double latency = Builder::to_seconds_double(now - data.created.time);
        //std::cout << "WorkerDataReaderListener::on_data_available() - Valid Data :: Latency = " << std::fixed << std::setprecision(6) << latency << " seconds" << std::endl;

        std::unique_lock<std::mutex> lock(mutex_);
        if (datareader_) {
          auto prev_latency_mean = latency_mean_->value.double_prop();
          auto prev_latency_var_x_sample_count = latency_var_x_sample_count_->value.double_prop();

          sample_count_->value.ull_prop(sample_count_->value.ull_prop() + 1);

          if (latency < latency_min_->value.double_prop()) {
            latency_min_->value.double_prop(latency);
          }
          if (latency_max_->value.double_prop() < latency) {
            latency_max_->value.double_prop(latency);
          }
          if (sample_count_->value.ull_prop() == 0) {
            latency_mean_->value.double_prop(latency);
            latency_var_x_sample_count_->value.double_prop(latency);
          } else {
            // Incremental mean calculation (doesn't require storing all the data)
            latency_mean_->value.double_prop(prev_latency_mean + ((latency - prev_latency_mean) / static_cast<double>(sample_count_->value.ull_prop())));
            // Incremental (variance * sample_count) calculation (doesn't require storing all the data, can be used to easily find variance / standard deviation)
            latency_var_x_sample_count_->value.double_prop(prev_latency_var_x_sample_count + ((latency - prev_latency_mean) * (latency - latency_mean_->value.double_prop())));
          }
        }
      }
    }
  }

  void on_subscription_matched(DDS::DataReader_ptr /*reader*/, const DDS::SubscriptionMatchedStatus& status) override {
    //std::cout << "WorkerDataReaderListener::on_subscription_matched" << std::endl;
    std::unique_lock<std::mutex> lock(mutex_);
    if (expected_count_ != 0) {
      if (static_cast<size_t>(status.current_count) == expected_count_) {
        //std::cout << "WorkerDataReaderListener reached expected count!" << std::endl;
        if (datareader_) {
          last_discovery_time_->value.time_prop(Builder::get_time());
        }
      }
    } else {
      if (static_cast<size_t>(status.current_count) > matched_count_) {
        if (datareader_) {
          last_discovery_time_->value.time_prop(Builder::get_time());
        }
      }
    }
    matched_count_ = status.current_count;
  }

  void on_sample_lost(DDS::DataReader_ptr /*reader*/, const DDS::SampleLostStatus& /*status*/) override {
  }

  void set_datareader(Builder::DataReader& datareader) override {
    datareader_ = &datareader;
    last_discovery_time_ = get_or_create_property(datareader_->get_report().properties, "last_discovery_time", Builder::PVK_TIME);

    sample_count_ = get_or_create_property(datareader_->get_report().properties, "sample_count", Builder::PVK_ULL);
    sample_count_->value.ull_prop(0);
    latency_min_ = get_or_create_property(datareader_->get_report().properties, "latency_min", Builder::PVK_DOUBLE);
    latency_min_->value.double_prop(std::numeric_limits<double>::max());
    latency_max_ = get_or_create_property(datareader_->get_report().properties, "latency_max", Builder::PVK_DOUBLE);
    latency_max_->value.double_prop(0.0);
    latency_mean_ = get_or_create_property(datareader_->get_report().properties, "latency_mean", Builder::PVK_DOUBLE);
    latency_mean_->value.double_prop(0.0);
    latency_var_x_sample_count_ = get_or_create_property(datareader_->get_report().properties, "latency_var_x_sample_count", Builder::PVK_DOUBLE);
    latency_var_x_sample_count_->value.double_prop(0.0);
  }

protected:
  std::mutex mutex_;
  size_t expected_count_{0};
  size_t matched_count_{0};
  Builder::DataReader* datareader_{0};
  Builder::PropertyIndex last_discovery_time_;
  Builder::PropertyIndex sample_count_;
  Builder::PropertyIndex latency_min_;
  Builder::PropertyIndex latency_max_;
  Builder::PropertyIndex latency_mean_;
  Builder::PropertyIndex latency_var_x_sample_count_;
};

// WorkerSubscriberListener

class WorkerSubscriberListener : public Builder::SubscriberListener {
public:

  // From DDS::DataReaderListener

  void on_requested_deadline_missed(DDS::DataReader_ptr /*reader*/, const DDS::RequestedDeadlineMissedStatus& /*status*/) override {
  }

  void on_requested_incompatible_qos(DDS::DataReader_ptr /*reader*/, const DDS::RequestedIncompatibleQosStatus& /*status*/) override {
  }

  void on_sample_rejected(DDS::DataReader_ptr /*reader*/, const DDS::SampleRejectedStatus& /*status*/) override {
  }

  void on_liveliness_changed(DDS::DataReader_ptr /*reader*/, const DDS::LivelinessChangedStatus& /*status*/) override {
  }

  void on_data_available(DDS::DataReader_ptr /*reader*/) override {
  }

  void on_subscription_matched(DDS::DataReader_ptr /*reader*/, const DDS::SubscriptionMatchedStatus& /*status*/) override {
  }

  void on_sample_lost(DDS::DataReader_ptr /*reader*/, const DDS::SampleLostStatus& /*status*/) override {
  }

  // From DDS::SubscriberListener

  void on_data_on_readers(DDS::Subscriber_ptr /*subs*/) override {
  }

  // From Builder::SubscriberListener

  void set_subscriber(Builder::Subscriber& subscriber) override {
    subscriber_ = &subscriber;
  }

protected:
  std::mutex mutex_;
  Builder::Subscriber* subscriber_{0};
};

// WorkerDataWriterListener

class WorkerDataWriterListener : public Builder::DataWriterListener {
public:

  WorkerDataWriterListener() {}
  WorkerDataWriterListener(size_t expected) : expected_count_(expected) {}

  void on_offered_deadline_missed(DDS::DataWriter_ptr /*writer*/, const DDS::OfferedDeadlineMissedStatus& /*status*/) override {
  }

  void on_offered_incompatible_qos(DDS::DataWriter_ptr /*writer*/, const DDS::OfferedIncompatibleQosStatus& /*status*/) override {
  }

  void on_liveliness_lost(DDS::DataWriter_ptr /*writer*/, const DDS::LivelinessLostStatus& /*status*/) override {
  }

  void on_publication_matched(DDS::DataWriter_ptr /*writer*/, const DDS::PublicationMatchedStatus& status) override {
    std::unique_lock<std::mutex> lock(mutex_);
    if (expected_count_ != 0) {
      if (static_cast<size_t>(status.current_count) == expected_count_) {
        //std::cout << "WorkerDataWriterListener reached expected count!" << std::endl;
        if (datawriter_) {
          last_discovery_time_->value.time_prop(Builder::get_time());
        }
      }
    } else {
      if (static_cast<size_t>(status.current_count) > matched_count_) {
        if (datawriter_) {
          last_discovery_time_->value.time_prop(Builder::get_time());
        }
      }
    }
    matched_count_ = status.current_count;
  }

  void set_datawriter(Builder::DataWriter& datawriter) override {
    datawriter_ = &datawriter;
    last_discovery_time_ = get_or_create_property(datawriter_->get_report().properties, "last_discovery_time", Builder::PVK_TIME);
  }

protected:
  std::mutex mutex_;
  size_t expected_count_{0};
  size_t matched_count_{0};
  Builder::DataWriter* datawriter_{0};
  Builder::PropertyIndex last_discovery_time_;
};

// WorkerPublisherListener

class WorkerPublisherListener : public Builder::PublisherListener {
public:

  // From DDS::DataWriterListener

  void on_offered_deadline_missed(DDS::DataWriter_ptr /*writer*/, const DDS::OfferedDeadlineMissedStatus& /*status*/) override {
  }

  void on_offered_incompatible_qos(DDS::DataWriter_ptr /*writer*/, const DDS::OfferedIncompatibleQosStatus& /*status*/) override {
  }

  void on_liveliness_lost(DDS::DataWriter_ptr /*writer*/, const DDS::LivelinessLostStatus& /*status*/) override {
  }

  void on_publication_matched(DDS::DataWriter_ptr /*writer*/, const DDS::PublicationMatchedStatus& /*status*/) override {
  }

  // From DDS::PublisherListener

  // From PublisherListener

  void set_publisher(Builder::Publisher& publisher) override {
    publisher_ = &publisher;
  }

protected:
  std::mutex mutex_;
  Builder::Publisher* publisher_{0};
};

// WorkerParticipantListener

class WorkerParticipantListener : public Builder::ParticipantListener {
public:

  // From DDS::DataWriterListener

  void on_offered_deadline_missed(DDS::DataWriter_ptr /*writer*/, const DDS::OfferedDeadlineMissedStatus& /*status*/) {
  }

  void on_offered_incompatible_qos(DDS::DataWriter_ptr /*writer*/, const DDS::OfferedIncompatibleQosStatus& /*status*/) {
  }

  void on_liveliness_lost(DDS::DataWriter_ptr /*writer*/, const DDS::LivelinessLostStatus& /*status*/) {
  }

  void on_publication_matched(DDS::DataWriter_ptr /*writer*/, const DDS::PublicationMatchedStatus& /*status*/) {
  }

  void on_requested_deadline_missed(DDS::DataReader_ptr /*reader*/, const DDS::RequestedDeadlineMissedStatus& /*status*/) {
  }

  void on_requested_incompatible_qos(DDS::DataReader_ptr /*reader*/, const DDS::RequestedIncompatibleQosStatus& /*status*/) {
  }

  // From DDS::SubscriberListener

  void on_data_on_readers(DDS::Subscriber_ptr /*subscriber*/) {
  }

  // From DDS::DataReaderListener

  void on_sample_rejected(DDS::DataReader_ptr /*reader*/, const DDS::SampleRejectedStatus& /*status*/) {
  }

  void on_liveliness_changed(DDS::DataReader_ptr /*reader*/, const DDS::LivelinessChangedStatus& /*status*/) {
  }

  void on_data_available(DDS::DataReader_ptr /*reader*/) {
  }

  void on_subscription_matched(DDS::DataReader_ptr /*reader*/, const DDS::SubscriptionMatchedStatus& /*status*/) {
  }

  void on_sample_lost(DDS::DataReader_ptr /*reader*/, const DDS::SampleLostStatus& /*status*/) {
  }

  void on_inconsistent_topic(DDS::Topic_ptr /*the_topic*/, const DDS::InconsistentTopicStatus& /*status*/) {
  }

  // From Builder::ParticipantListener

  void set_participant(Builder::Participant& participant) override {
    participant_ = &participant;
  }

protected:
  std::mutex mutex_;
  Builder::Participant* participant_{0};
};

using Builder::ReaderMap;
using Builder::WriterMap;

template <typename T>
class ProAction : public ACE_Handler {
public:
  ProAction(T&& to_call) : to_call_(to_call) {}
  virtual ~ProAction() {}
  ProAction(const ProAction&) = delete;

  void handle_time_out(const ACE_Time_Value& /*tv*/, const void* /*act*/) override {
    to_call_();
  }

protected:
  T to_call_;
};

// WriteAction
class WriteAction : public Action {
public:
  WriteAction(ACE_Proactor& proactor);

  bool init(const ActionConfig& config, ActionReport& report, Builder::ReaderMap& readers, Builder::WriterMap& writers) override;

  void start() override;
  void stop() override;

  void do_write();

protected:
  std::mutex mutex_;
  ACE_Proactor& proactor_;
  bool started_, stopped_;
  DataDataWriter_var data_dw_;
  Data data_;
  ACE_Time_Value write_period_;
  DDS::InstanceHandle_t instance_;
  std::shared_ptr<ACE_Handler> handler_;
};

WriteAction::WriteAction(ACE_Proactor& proactor) : proactor_(proactor), started_(false), stopped_(false), write_period_(1, 0) {
}

uint32_t one_at_a_time_hash(const uint8_t* key, size_t length) {
  size_t i = 0;
  uint32_t hash = 0;
  while (i != length) {
    hash += key[i++];
    hash += hash << 10;
    hash ^= hash >> 6;
  }
  hash += hash << 3;
  hash ^= hash >> 11;
  hash += hash << 15;
  return hash;
}

bool WriteAction::init(const ActionConfig& config, ActionReport& report, Builder::ReaderMap& readers, Builder::WriterMap& writers) {
  std::unique_lock<std::mutex> lock(mutex_);
  Action::init(config, report, readers, writers);
  if (writers_by_index_.empty()) {
    throw std::runtime_error("WriteAction is missing a writer");
  }
  std::string name(config.name.in());
  data_.key = one_at_a_time_hash(reinterpret_cast<const uint8_t*>(name.data()), name.size());

  size_t data_buffer_bytes = 256;
  auto data_buffer_bytes_prop = get_property(config.params, "data_buffer_bytes", Builder::PVK_ULL);
  if (data_buffer_bytes_prop) {
std::cout << "found data_buffer_bytes!" << std::endl;
    data_buffer_bytes = data_buffer_bytes_prop->value.ull_prop();
  }
  data_.buffer.length(data_buffer_bytes);

  auto write_period_prop = get_property(config.params, "write_period", Builder::PVK_TIME);
  if (write_period_prop) {
std::cout << "found write period!" << std::endl;
    write_period_ = ACE_Time_Value(write_period_prop->value.time_prop().sec, write_period_prop->value.time_prop().nsec / 1e3);
  }

  data_dw_ = DataDataWriter::_narrow(writers_by_index_[0]->get_dds_datawriter());
  if (data_dw_) {
    handler_.reset(new ProAction<decltype(std::bind(&WriteAction::do_write, this))>(std::bind(&WriteAction::do_write, this)));
  }

  return true;
}

void WriteAction::start() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!started_) {
    instance_ = data_dw_->register_instance(data_);
    started_ = true;
    proactor_.schedule_timer(*handler_, nullptr, write_period_);
  }
}

void WriteAction::stop() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (started_ && !stopped_) {
    stopped_ = true;
    proactor_.cancel_timer(*handler_);
    data_dw_->unregister_instance(data_, instance_);
  }
}

void WriteAction::do_write() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (started_ && !stopped_) {
    data_.created.time = Builder::get_time();
    DDS::ReturnCode_t result = data_dw_->write(data_, 0);
    if (result != DDS::RETCODE_OK) {
      std::cout << "Error during WriteAction::do_write()'s call to datawriter::write()" << std::endl;
    }
    proactor_.schedule_timer(*handler_, nullptr, write_period_);
  }
}

} // namespace Stool

// Main

int ACE_TMAIN(int argc, ACE_TCHAR* argv[]) {

  if (argc < 2) {
    std::cerr << "Configuration file expected as second argument." << std::endl;
    return 1;
  }

  std::ifstream ifs(argv[1]);
  if (!ifs.good()) {
    std::cerr << "Unable to open configuration file: '" << argv[1] << "'" << std::endl;
    return 2;
  }

  using Builder::ZERO;

  Stool::WorkerConfig config;

  config.enable_time = ZERO;
  config.start_time = ZERO;
  config.stop_time = ZERO;

  if (!json_2_builder(ifs, config)) {
    std::cerr << "Unable to parse configuration file: '" << argv[1] << "'" << std::endl;
    return 3;
  }

  /* Manually building the ProcessConfig object

  config.config_sections.length(1);
  config.config_sections[0].name = "common";
  config.config_sections[0].properties.length(2);
  config.config_sections[0].properties[0].name = "DCPSSecurity";
  config.config_sections[0].properties[0].value = "0";
  config.config_sections[0].properties[1].name = "DCPSDebugLevel";
  config.config_sections[0].properties[1].value = "0";

  config.discoveries.length(1);
  config.discoveries[0].type = "rtps";
  config.discoveries[0].name = "stool_test_rtps";
  config.discoveries[0].domain = 7;

  const size_t ips_per_proc_max = 10;
  const size_t subs_per_ip_max = 5;
  const size_t pubs_per_ip_max = 5;
  const size_t drs_per_sub_max = 10;
  const size_t dws_per_pub_max = 10;

  const size_t expected_datareader_matches = ips_per_proc_max * pubs_per_ip_max * dws_per_pub_max;
  const size_t expected_datawriter_matches = ips_per_proc_max * subs_per_ip_max * drs_per_sub_max;

  config.instances.length(ips_per_proc_max);
  config.participants.length(ips_per_proc_max);

  for (size_t ip = 0; ip < ips_per_proc_max; ++ip) {
    std::stringstream instance_name_ss;
    instance_name_ss << "transport_" << ip << std::flush;
    config.instances[ip].type = "rtps_udp";
    config.instances[ip].name = instance_name_ss.str().c_str();
    config.instances[ip].domain = 7;
    std::stringstream participant_name_ss;
    participant_name_ss << "participant_" << ip << std::flush;
    config.participants[ip].name = participant_name_ss.str().c_str();
    config.participants[ip].domain = 7;
    config.participants[ip].listener_type_name = "stool_partl";
    config.participants[ip].listener_status_mask = OpenDDS::DCPS::DEFAULT_STATUS_MASK;
    //config.participants[ip].type_names.length(1);
    //config.participants[ip].type_names[0] = "Stool::Data";
    config.participants[ip].transport_config_name = instance_name_ss.str().c_str();

    config.participants[ip].qos.entity_factory.autoenable_created_entities = false;
    config.participants[ip].qos_mask.entity_factory.has_autoenable_created_entities = false;

    config.participants[ip].topics.length(1);
    std::stringstream topic_name_ss;
    topic_name_ss << "topic" << std::flush;
    config.participants[ip].topics[0].name = topic_name_ss.str().c_str();
    //config.participants[ip].topics[0].type_name = "Stool::Data";
    config.participants[ip].topics[0].listener_type_name = "stool_tl";
    config.participants[ip].topics[0].listener_status_mask = OpenDDS::DCPS::DEFAULT_STATUS_MASK;

    config.participants[ip].subscribers.length(subs_per_ip_max);
    for (size_t sub = 0; sub < subs_per_ip_max; ++sub) {
      std::stringstream subscriber_name_ss;
      subscriber_name_ss << "subscriber_" << ip << "_" << sub << std::flush;
      config.participants[ip].subscribers[sub].name = subscriber_name_ss.str().c_str();
      config.participants[ip].subscribers[sub].listener_type_name = "stool_sl";
      config.participants[ip].subscribers[sub].listener_status_mask = OpenDDS::DCPS::DEFAULT_STATUS_MASK;

      config.participants[ip].subscribers[sub].datareaders.length(drs_per_sub_max);
      for (size_t dr = 0; dr < drs_per_sub_max; ++dr) {
        std::stringstream datareader_name_ss;
        datareader_name_ss << "datareader_" << ip << "_" << sub << "_" << dr << std::flush;
        config.participants[ip].subscribers[sub].datareaders[dr].name = datareader_name_ss.str().c_str();
        config.participants[ip].subscribers[sub].datareaders[dr].topic_name = topic_name_ss.str().c_str();
        config.participants[ip].subscribers[sub].datareaders[dr].listener_type_name = "stool_drl";
        config.participants[ip].subscribers[sub].datareaders[dr].listener_status_mask = OpenDDS::DCPS::DEFAULT_STATUS_MASK;

        config.participants[ip].subscribers[sub].datareaders[dr].qos.reliability.kind = DDS::RELIABLE_RELIABILITY_QOS;
        config.participants[ip].subscribers[sub].datareaders[dr].qos_mask.reliability.has_kind = true;
      }
    }
    config.participants[ip].publishers.length(pubs_per_ip_max);
    for (size_t pub = 0; pub < pubs_per_ip_max; ++pub) {
      std::stringstream publisher_name_ss;
      publisher_name_ss << "publisher_" << ip << "_" << pub << std::flush;
      config.participants[ip].publishers[pub].name = publisher_name_ss.str().c_str();
      config.participants[ip].publishers[pub].listener_type_name = "stool_pl";
      config.participants[ip].publishers[pub].listener_status_mask = OpenDDS::DCPS::DEFAULT_STATUS_MASK;

      config.participants[ip].publishers[pub].datawriters.length(dws_per_pub_max);
      for (size_t dw = 0; dw < dws_per_pub_max; ++dw) {
        std::stringstream datawriter_name_ss;
        datawriter_name_ss << "datawriter_" << ip << "_" << pub << "_" << dw << std::flush;
        config.participants[ip].publishers[pub].datawriters[dw].name = datawriter_name_ss.str().c_str();
        config.participants[ip].publishers[pub].datawriters[dw].topic_name = topic_name_ss.str().c_str();
        config.participants[ip].publishers[pub].datawriters[dw].listener_type_name = "stool_dwl";
        config.participants[ip].publishers[pub].datawriters[dw].listener_status_mask = OpenDDS::DCPS::DEFAULT_STATUS_MASK;
      }
    }
  }
  */

  // Register some Stool-specific types
  Builder::TypeSupportRegistry::TypeSupportRegistration process_config_registration(new Builder::ProcessConfigTypeSupportImpl());
  Builder::TypeSupportRegistry::TypeSupportRegistration data_registration(new Stool::DataTypeSupportImpl());

  // Register some Stool-specific listener factories
  Builder::ListenerFactory<DDS::TopicListener>::Registration topic_registration("stool_tl", [](){ return DDS::TopicListener_var(new Stool::WorkerTopicListener()); });
  Builder::ListenerFactory<DDS::DataReaderListener>::Registration datareader_registration("stool_drl", [&](){ return DDS::DataReaderListener_var(new Stool::WorkerDataReaderListener()); });
  Builder::ListenerFactory<DDS::SubscriberListener>::Registration subscriber_registration("stool_sl", [](){ return DDS::SubscriberListener_var(new Stool::WorkerSubscriberListener()); });
  Builder::ListenerFactory<DDS::DataWriterListener>::Registration datawriter_registration("stool_dwl", [&](){ return DDS::DataWriterListener_var(new Stool::WorkerDataWriterListener()); });
  Builder::ListenerFactory<DDS::PublisherListener>::Registration publisher_registration("stool_pl", [](){ return DDS::PublisherListener_var(new Stool::WorkerPublisherListener()); });
  Builder::ListenerFactory<DDS::DomainParticipantListener>::Registration participant_registration("stool_partl", [](){ return DDS::DomainParticipantListener_var(new Stool::WorkerParticipantListener()); });

  // Disable some Proactor debug chatter to std out (eventually make this configurable?)
  ACE_Log_Category::ace_lib().priority_mask(0);

  ACE_Proactor proactor;

  // Register actions
  Stool::ActionManager::Registration write_action_registration("write", [&](){ return std::shared_ptr<Stool::Action>(new Stool::WriteAction(proactor)); });

  // Timestamps used to measure method call durations
  Builder::TimeStamp process_construction_begin_time = ZERO, process_construction_end_time = ZERO;
  Builder::TimeStamp process_enable_begin_time = ZERO, process_enable_end_time = ZERO;
  Builder::TimeStamp process_start_begin_time = ZERO, process_start_end_time = ZERO;
  Builder::TimeStamp process_stop_begin_time = ZERO, process_stop_end_time = ZERO;
  Builder::TimeStamp process_destruction_begin_time = ZERO, process_destruction_end_time = ZERO;

  Builder::ProcessReport process_report;

  const size_t THREAD_POOL_SIZE = 4;
  std::vector<std::shared_ptr<std::thread> > thread_pool;
  for (size_t i = 0; i < THREAD_POOL_SIZE; ++i) {
    thread_pool.emplace_back(std::make_shared<std::thread>([&](){ proactor.proactor_run_event_loop(); }));
  }

  try {
    std::string line;
    std::condition_variable cv;
    std::mutex cv_mutex;

    std::cout << "Beginning process construction / entity creation." << std::endl;

    process_construction_begin_time = Builder::get_time();
    Builder::Process process(config.process);
    process_construction_end_time = Builder::get_time();

    std::cout << std::endl << "Process construction / entity creation complete." << std::endl << std::endl;

    std::cout << "Beginning action construction / initialization." << std::endl;

    Stool::ActionManager am(config.actions, config.action_reports, process.get_reader_map(), process.get_writer_map());

    std::cout << "Action construction / initialization complete." << std::endl << std::endl;

    if (config.enable_time == ZERO) {
      std::cout << "No test enable time specified. Press any key to enable process entities." << std::endl;
      std::getline(std::cin, line);
    } else {
      if (config.enable_time < ZERO) {
        auto dur = -get_duration(config.enable_time);
        std::unique_lock<std::mutex> lock(cv_mutex);
        while (cv.wait_for(lock, dur) != std::cv_status::timeout) {}
      } else {
        auto timeout_time = std::chrono::system_clock::time_point(get_duration(config.enable_time));
        std::unique_lock<std::mutex> lock(cv_mutex);
        while (cv.wait_until(lock, timeout_time) != std::cv_status::timeout) {}
      }
    }

    std::cout << "Enabling DDS entities (if not already enabled)." << std::endl;

    process_enable_begin_time = Builder::get_time();
    process.enable_dds_entities();
    process_enable_end_time = Builder::get_time();

    std::cout << "DDS entities enabled." << std::endl << std::endl;

    if (config.start_time == ZERO) {
      std::cout << "No test start time specified. Press any key to start process testing." << std::endl;
      std::getline(std::cin, line);
    } else {
      if (config.start_time < ZERO) {
        auto dur = -get_duration(config.start_time);
        std::unique_lock<std::mutex> lock(cv_mutex);
        while (cv.wait_for(lock, dur) != std::cv_status::timeout) {}
      } else {
        auto timeout_time = std::chrono::system_clock::time_point(get_duration(config.start_time));
        std::unique_lock<std::mutex> lock(cv_mutex);
        while (cv.wait_until(lock, timeout_time) != std::cv_status::timeout) {}
      }
    }

    std::cout << "Starting process tests." << std::endl;

    process_start_begin_time = Builder::get_time();
    am.start();
    process_start_end_time = Builder::get_time();

    std::cout << "Process tests started." << std::endl << std::endl;

    if (config.stop_time == ZERO) {
      std::cout << "No stop time specified. Press any key to stop process testing." << std::endl;
      std::getline(std::cin, line);
    } else {
      if (config.stop_time < ZERO) {
        auto dur = -get_duration(config.stop_time);
        std::unique_lock<std::mutex> lock(cv_mutex);
        while (cv.wait_for(lock, dur) != std::cv_status::timeout) {}
      } else {
        auto timeout_time = std::chrono::system_clock::time_point(get_duration(config.stop_time));
        std::unique_lock<std::mutex> lock(cv_mutex);
        while (cv.wait_until(lock, timeout_time) != std::cv_status::timeout) {}
      }
    }

    std::cout << "Stopping process tests." << std::endl;

    process_stop_begin_time = Builder::get_time();
    am.stop();
    process_stop_end_time = Builder::get_time();

    std::cout << "Process tests stopped." << std::endl << std::endl;

    proactor.proactor_end_event_loop();
    for (size_t i = 0; i < THREAD_POOL_SIZE; ++i) {
      thread_pool[i]->join();
    }
    thread_pool.clear();

    process_report = process.get_report();

    std::cout << "Beginning process destruction / entity deletion." << std::endl;

    process_destruction_begin_time = Builder::get_time();
  } catch (const std::exception& e) {
    std::cerr << "Exception caught trying to build process object: " << e.what() << std::endl;
    return 1;
  } catch (...) {
    std::cerr << "Unknown exception caught trying to build process object" << std::endl;
    return 1;
  }
  process_destruction_end_time = Builder::get_time();

  std::cout << "Process destruction / entity deletion complete." << std::endl << std::endl;

  // Some preliminary measurements and reporting (eventually will shift to another process?)
  Stool::WorkerReport worker_report;
  worker_report.undermatched_readers = 0;
  worker_report.undermatched_writers = 0;
  worker_report.max_discovery_time_delta = ZERO;
  worker_report.sample_count = 0;
  worker_report.latency_min = std::numeric_limits<double>::max();
  worker_report.latency_max = 0.0;
  worker_report.latency_mean = 0.0;
  worker_report.latency_var_x_sample_count = 0.0;

  for (CORBA::ULong i = 0; i < process_report.participants.length(); ++i) {
    for (CORBA::ULong j = 0; j < process_report.participants[i].subscribers.length(); ++j) {
      for (CORBA::ULong k = 0; k < process_report.participants[i].subscribers[j].datareaders.length(); ++k) {
        Builder::DataReaderReport& dr_report = process_report.participants[i].subscribers[j].datareaders[k];
        const Builder::TimeStamp dr_enable_time = get_or_create_property(dr_report.properties, "enable_time", Builder::PVK_TIME)->value.time_prop();
        const Builder::TimeStamp dr_last_discovery_time = get_or_create_property(dr_report.properties, "last_discovery_time", Builder::PVK_TIME)->value.time_prop();
        const CORBA::ULongLong dr_sample_count = get_or_create_property(dr_report.properties, "sample_count", Builder::PVK_ULL)->value.ull_prop();
        const double dr_latency_min = get_or_create_property(dr_report.properties, "latency_min", Builder::PVK_DOUBLE)->value.double_prop();
        const double dr_latency_max = get_or_create_property(dr_report.properties, "latency_max", Builder::PVK_DOUBLE)->value.double_prop();
        const double dr_latency_mean = get_or_create_property(dr_report.properties, "latency_mean", Builder::PVK_DOUBLE)->value.double_prop();
        const double dr_latency_var_x_sample_count = get_or_create_property(dr_report.properties, "latency_var_x_sample_count", Builder::PVK_DOUBLE)->value.double_prop();
        if (ZERO < dr_enable_time && ZERO < dr_last_discovery_time) {
          auto delta = dr_last_discovery_time - dr_enable_time;
          if (worker_report.max_discovery_time_delta < delta) {
            worker_report.max_discovery_time_delta = delta;
          }
        } else {
          ++worker_report.undermatched_readers;
        }
        if (dr_latency_min < worker_report.latency_min) {
          worker_report.latency_min = dr_latency_min;
        }
        if (worker_report.latency_max < dr_latency_max) {
          worker_report.latency_max = dr_latency_max;
        }
        if ((worker_report.sample_count + dr_sample_count) > 0) {
          worker_report.latency_mean = (worker_report.latency_mean * static_cast<double>(worker_report.sample_count) + dr_latency_mean * static_cast<double>(dr_sample_count)) / (worker_report.sample_count + dr_sample_count);
        }
        worker_report.latency_var_x_sample_count += dr_latency_var_x_sample_count;
        worker_report.sample_count += dr_sample_count;
      }
    }
    for (CORBA::ULong j = 0; j < process_report.participants[i].publishers.length(); ++j) {
      for (CORBA::ULong k = 0; k < process_report.participants[i].publishers[j].datawriters.length(); ++k) {
        Builder::DataWriterReport& dw_report = process_report.participants[i].publishers[j].datawriters[k];
        const Builder::TimeStamp dw_enable_time = get_or_create_property(dw_report.properties, "enable_time", Builder::PVK_TIME)->value.time_prop();
        const Builder::TimeStamp dw_last_discovery_time = get_or_create_property(dw_report.properties, "last_discovery_time", Builder::PVK_TIME)->value.time_prop();
        if (ZERO < dw_enable_time && ZERO < dw_last_discovery_time) {
          auto delta = dw_last_discovery_time - dw_enable_time;
          if (worker_report.max_discovery_time_delta < delta) {
            worker_report.max_discovery_time_delta = delta;
          }
        } else {
          ++worker_report.undermatched_writers;
        }
      }
    }
  }

  std::cout << std::endl << "--- Process Statistics ---" << std::endl << std::endl;

  std::cout << "construction_time: " << process_construction_end_time - process_construction_begin_time << std::endl;
  std::cout << "enable_time: " << process_enable_end_time - process_enable_begin_time << std::endl;
  std::cout << "start_time: " << process_start_end_time - process_start_begin_time << std::endl;
  std::cout << "stop_time: " << process_stop_end_time - process_stop_begin_time << std::endl;
  std::cout << "destruction_time: " << process_destruction_end_time - process_destruction_begin_time << std::endl;

  std::cout << std::endl << "--- Discovery Statistics ---" << std::endl << std::endl;

  std::cout << "undermatched readers: " << worker_report.undermatched_readers << ", undermatched writers: " << worker_report.undermatched_writers << std::endl << std::endl;
  std::cout << "max_discovery_time_delta: " << worker_report.max_discovery_time_delta << std::endl;

  if (worker_report.sample_count > 0) {
    std::cout << std::endl << "--- Latency Statistics ---" << std::endl << std::endl;

    std::cout << "total sample count: " << worker_report.sample_count << std::endl;
    std::cout << "minimum latency: " << std::fixed << std::setprecision(6) << worker_report.latency_min << " seconds" << std::endl;
    std::cout << "maximum latency: " << std::fixed << std::setprecision(6) << worker_report.latency_max << " seconds" << std::endl;
    std::cout << "mean latency: " << std::fixed << std::setprecision(6) << worker_report.latency_mean << " seconds" << std::endl;
    std::cout << "latency standard deviation: " << std::fixed << std::setprecision(6) << std::sqrt(worker_report.latency_var_x_sample_count / static_cast<double>(worker_report.sample_count)) << " seconds" << std::endl;
    std::cout << std::endl;
  }

  return 0;
}

